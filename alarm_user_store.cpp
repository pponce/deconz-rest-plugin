#include "alarm_user_store.h"
#include <sqlite3.h>
#include <algorithm>
#include <utility>

namespace AlarmUsers {
namespace {
struct Statement {
    sqlite3_stmt *s = nullptr;
    Statement(sqlite3 *db, const char *sql) { sqlite3_prepare_v2(db, sql, -1, &s, nullptr); }
    ~Statement() { sqlite3_finalize(s); }
    bool valid() const { return s != nullptr; }
    void number(int n, int64_t v) { sqlite3_bind_int64(s, n, v); }
    void text(int n, const std::string &v) { sqlite3_bind_text(s, n, v.data(), int(v.size()), SQLITE_TRANSIENT); }
    int step() { return s ? sqlite3_step(s) : SQLITE_ERROR; }
    int64_t number(int n) { return sqlite3_column_int64(s, n); }
    std::string text(int n) { const auto *v = sqlite3_column_text(s, n); return v ? reinterpret_cast<const char *>(v) : ""; }
};
bool exec(sqlite3 *db, const char *sql) { return db && sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK; }
struct Transaction {
    sqlite3 *db;
    bool active;
    explicit Transaction(sqlite3 *d) : db(d), active(exec(d, "BEGIN IMMEDIATE")) {}
    ~Transaction() { if (active) exec(db, "ROLLBACK"); }
    bool commit() { if (!active || !exec(db, "COMMIT")) return false; active = false; return true; }
};
bool validPin(const std::string &pin) {
    return pin.size() >= 4 && pin.size() <= 16 &&
        std::all_of(pin.begin(), pin.end(), [](char c) { return c >= '0' && c <= '9'; });
}
std::string legacyKey(int alarm) { return "as_" + std::to_string(alarm) + "_code0"; }
bool mirror(sqlite3 *db, int alarm, const User &u) {
    if (u.slot != 0) return true;
    Statement s(db, "INSERT OR REPLACE INTO secrets(uniqueid,secret,state) VALUES(?,?,?)");
    if (!s.valid()) return false;
    s.text(1, legacyKey(alarm)); s.text(2, u.hash); s.number(3, u.enabled ? 1 : 0);
    return s.step() == SQLITE_DONE;
}
}
Store::Store(sqlite3 *d, Verify v, Hash h) : db(d), verify(std::move(v)), hash(std::move(h)) {}

bool Store::init(int alarm) {
    if (!db || alarm < 1 || alarm > 255) return false;
    // Additive schema. No changes to upstream table layout or user_version.
    if (!exec(db, "CREATE TABLE IF NOT EXISTS alarm_users_v1 ("
        "alarm INTEGER NOT NULL, slot INTEGER NOT NULL CHECK(slot BETWEEN 0 AND 8),"
        "uid TEXT NOT NULL UNIQUE, name TEXT NOT NULL, hash TEXT NOT NULL,"
        "enabled INTEGER NOT NULL CHECK(enabled IN (0,1)),"
        "remaining INTEGER NOT NULL CHECK(remaining>=-1), revision INTEGER NOT NULL,"
        "PRIMARY KEY(alarm,slot))") ||
        !exec(db, "CREATE TABLE IF NOT EXISTS alarm_user_requests_v1 ("
        "alarm INTEGER NOT NULL,source TEXT NOT NULL,endpoint INTEGER NOT NULL,"
        "sequence INTEGER NOT NULL,mode INTEGER NOT NULL,created INTEGER NOT NULL,"
        "uid TEXT NOT NULL,response INTEGER NOT NULL,eventid TEXT NOT NULL,"
        "PRIMARY KEY(alarm,source,endpoint,sequence))")) return false;
    Statement s(db, "INSERT OR IGNORE INTO alarm_users_v1 "
        "SELECT ?,0,lower(hex(randomblob(16))),'Main',secret,CASE WHEN state=1 THEN 1 ELSE 0 END,-1,1 "
        "FROM secrets WHERE uniqueid=? AND length(secret)>0");
    if (!s.valid()) return false;
    s.number(1, alarm); s.text(2, legacyKey(alarm));
    return s.step() == SQLITE_DONE;
}
bool Store::read(int alarm, std::vector<User> &users) {
    users.clear();
    Statement s(db, "SELECT slot,uid,name,hash,enabled,remaining,revision FROM alarm_users_v1 WHERE alarm=? ORDER BY slot");
    if (!s.valid()) return false;
    s.number(1, alarm);
    int rc;
    while ((rc = s.step()) == SQLITE_ROW) {
        User u; u.slot = int(s.number(0)); u.id = s.text(1); u.name = s.text(2); u.hash = s.text(3);
        u.enabled = s.number(4) != 0; u.remaining = s.number(5); u.revision = s.number(6);
        users.push_back(u);
    }
    return rc == SQLITE_DONE;
}
bool Store::list(int alarm, std::vector<User> &users) {
    Transaction t(db);
    return t.active && init(alarm) && read(alarm, users) && t.commit();
}
bool Store::put(int alarm, User &u, const std::string &pin, int64_t revision, std::string &error) {
    error = "invalid_user";
    if (u.slot < 0 || u.slot >= MaxUsers || u.name.empty() || u.name.size() > 64 ||
        std::any_of(u.name.begin(), u.name.end(), [](unsigned char c) { return c < 32 || c == 127; }) ||
        u.remaining < -1 || u.remaining > 1000000 || revision < 0 || (!pin.empty() && !validPin(pin))) return false;
    error = "storage_error";
    Transaction t(db); std::vector<User> users;
    if (!t.active || !init(alarm) || !read(alarm, users)) return false;
    auto old = std::find_if(users.begin(), users.end(), [&](const User &x) { return x.slot == u.slot; });
    if ((old == users.end() && revision != 0) || (old != users.end() && old->revision != revision)) {
        error = "revision_conflict"; return false;
    }
    if (pin.empty() && old == users.end()) { error = "pin_required"; return false; }
    if (!pin.empty()) {
        for (const auto &x : users) if (x.slot != u.slot && verify(x.hash, pin)) {
            error = "pin_already_assigned"; return false;
        }
        u.hash = hash(pin);
        if (u.hash.empty()) return false;
    } else u.hash = old->hash;
    Statement s(db, "INSERT OR REPLACE INTO alarm_users_v1(alarm,slot,uid,name,hash,enabled,remaining,revision) "
        "VALUES(?,?,coalesce((SELECT uid FROM alarm_users_v1 WHERE alarm=? AND slot=?),lower(hex(randomblob(16)))),?,?,?,?,?)");
    if (!s.valid()) return false;
    s.number(1,alarm); s.number(2,u.slot); s.number(3,alarm); s.number(4,u.slot);
    s.text(5,u.name); s.text(6,u.hash); s.number(7,u.enabled); s.number(8,u.remaining); s.number(9,revision+1);
    if (s.step()!=SQLITE_DONE || !mirror(db,alarm,u) || !read(alarm,users) || !t.commit()) return false;
    u = *std::find_if(users.begin(),users.end(),[&](const User &x){return x.slot==u.slot;});
    error.clear(); return true;
}
bool Store::erase(int alarm, int slot, int64_t revision) {
    if (slot < 0 || slot >= MaxUsers || revision < 1) return false;
    Transaction t(db);
    if (!t.active || !init(alarm)) return false;
    Statement s(db,"DELETE FROM alarm_users_v1 WHERE alarm=? AND slot=? AND revision=?");
    if (!s.valid()) return false;
    s.number(1,alarm); s.number(2,slot); s.number(3,revision);
    if (s.step()!=SQLITE_DONE || sqlite3_changes(db)!=1) return false;
    if (slot == 0) {
        Statement d(db,"DELETE FROM secrets WHERE uniqueid=?");
        if (!d.valid()) return false;
        d.text(1,legacyKey(alarm)); if (d.step()!=SQLITE_DONE) return false;
    }
    return t.commit();
}
bool Store::mainCode(int alarm, const std::string &pin) {
    std::vector<User> users;
    if (!list(alarm,users)) return false;
    for (const auto &u:users) if (u.slot==0) return u.enabled && u.remaining!=0 && verify(u.hash,pin);
    return false;
}
bool Store::setMainCode(int alarm, const std::string &pin) {
    std::vector<User> users;
    if (!list(alarm,users)) return false;
    User u; u.slot=0; u.name="Main";
    for (const auto &x:users) if (x.slot==0) u=x;
    std::string error;
    return put(alarm,u,pin,u.revision,error); // preserves disabled/exhausted status
}
Result Store::authorize(int alarm,const std::string &source,int endpoint,int sequence,
                        int mode,const std::string &pin,int64_t now,bool alreadyDisarmed) {
    Result result;
    if (source.empty() || source.size()>32 || endpoint<1 || endpoint>240 || sequence<0 || sequence>255 ||
        mode<0 || mode>3 || now<=DuplicateWindowMs) return result;
    Transaction t(db); std::vector<User> users;
    if (!t.active || !init(alarm) || !read(alarm,users)) return result;
    // Match disabled/exhausted users too, so retries of the final use can be recognized.
    User matched;
    for (const auto &u:users) if (verify(u.hash,pin)) { matched=u; break; }
    Statement previous(db,"SELECT mode,created,uid,response,eventid FROM alarm_user_requests_v1 "
        "WHERE alarm=? AND source=? AND endpoint=? AND sequence=?");
    if (!previous.valid()) return result;
    previous.number(1,alarm); previous.text(2,source); previous.number(3,endpoint); previous.number(4,sequence);
    int rc=previous.step();
    if (rc==SQLITE_ROW) {
        const auto age=now-previous.number(1);
        if (age<0) return result; // clock moved backward: never re-admit uncertain receipt
        if (age<=DuplicateWindowMs) {
            if (previous.number(0)!=mode || previous.text(2)!=matched.id) return result;
            result.response=int(previous.number(3)); result.eventId=previous.text(4);
            result.user=matched; result.duplicate=true; result.ok=t.commit(); return result;
        }
    } else if (rc!=SQLITE_DONE) return result;
    previous.step(); // finish read before writes/commit
    result.response=4;
    if (matched.slot>=0 && matched.enabled && matched.remaining!=0) {
        result.response=(mode==0 && alreadyDisarmed)?6:mode;
        // Only accepted physical keypad DISARM consumes a use. REST and ARM do not.
        if (mode==0 && matched.remaining>0) --matched.remaining;
        Statement consume(db,"UPDATE alarm_users_v1 SET remaining=?,revision=revision+1 WHERE alarm=? AND slot=? AND revision=?");
        if (!consume.valid()) return Result{};
        consume.number(1,matched.remaining); consume.number(2,alarm); consume.number(3,matched.slot); consume.number(4,matched.revision);
        if (consume.step()!=SQLITE_DONE || sqlite3_changes(db)!=1) return Result{};
        ++matched.revision; result.user=matched;
    }
    Statement clean(db,"DELETE FROM alarm_user_requests_v1 WHERE created<?");
    if (!clean.valid()) return Result{};
    clean.number(1,now-DuplicateWindowMs);
    if (clean.step()!=SQLITE_DONE) return Result{};
    Statement save(db,"INSERT OR REPLACE INTO alarm_user_requests_v1 VALUES(?,?,?,?,?,?,?,?,lower(hex(randomblob(16))))");
    if (!save.valid()) return Result{};
    save.number(1,alarm); save.text(2,source); save.number(3,endpoint); save.number(4,sequence);
    save.number(5,mode); save.number(6,now); save.text(7,matched.id); save.number(8,result.response);
    if (save.step()!=SQLITE_DONE) return Result{};
    Statement id(db,"SELECT eventid FROM alarm_user_requests_v1 WHERE alarm=? AND source=? AND endpoint=? AND sequence=?");
    if (!id.valid()) return Result{};
    id.number(1,alarm); id.text(2,source); id.number(3,endpoint); id.number(4,sequence);
    if (id.step()!=SQLITE_ROW) return Result{};
    result.eventId=id.text(0); id.step();
    result.ok=t.commit(); return result;
}
}
