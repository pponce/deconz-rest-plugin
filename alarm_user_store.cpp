#include "alarm_user_store.h"
#include <sqlite3.h>
#include <algorithm>
#include <utility>
#include <chrono>

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
Store::Store(sqlite3 *d, Verify v, Hash h, ScheduleCheck check)
    : db(d), verify(std::move(v)), hash(std::move(h)), scheduleCheck(std::move(check)) {}

bool Store::managementEnabled(int alarm, bool &enabled) {
    enabled = false;
    if (!db || alarm < 1 || alarm > 255) return false;
    Statement exists(db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name='alarm_user_management_v1'");
    if (!exists.valid()) return false;
    const int rc = exists.step();
    if (rc == SQLITE_DONE) return true; // Existing installations need no schema migration.
    if (rc != SQLITE_ROW) return false;
    Statement s(db, "SELECT 1 FROM alarm_user_management_v1 WHERE alarm=?");
    if (!s.valid()) return false;
    s.number(1, alarm);
    const int found = s.step();
    enabled = found == SQLITE_ROW;
    return enabled || found == SQLITE_DONE;
}

bool Store::activate(int alarm) {
    Statement s(db, "INSERT OR IGNORE INTO alarm_user_management_v1 VALUES(?)");
    if (!s.valid()) return false;
    s.number(1, alarm);
    return s.step() == SQLITE_DONE;
}

bool Store::init(int alarm) {
    if (!db || alarm < 1 || alarm > 255) return false;
    // Additive schema. No changes to upstream table layout or user_version.
    if (!exec(db, "CREATE TABLE IF NOT EXISTS alarm_user_management_v1 (alarm INTEGER PRIMARY KEY)") ||
        !exec(db, "CREATE TABLE IF NOT EXISTS alarm_users_v1 ("
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
    if (!exec(db, "CREATE TABLE IF NOT EXISTS alarm_user_schedules_v1 (uid TEXT PRIMARY KEY, policy TEXT NOT NULL)")) return false;
    bool permissionColumn = false;
    {
        Statement columns(db, "PRAGMA table_info(alarm_users_v1)");
        if (!columns.valid()) return false;
        int rc;
        while ((rc = columns.step()) == SQLITE_ROW)
            if (columns.text(1) == "api_arm_disarm") permissionColumn = true;
        if (rc != SQLITE_DONE) return false;
    }
    if (!permissionColumn) {
        if (!exec(db, "ALTER TABLE alarm_users_v1 ADD COLUMN api_arm_disarm INTEGER NOT NULL DEFAULT 0 CHECK(api_arm_disarm IN (0,1))") ||
            !exec(db, "UPDATE alarm_users_v1 SET api_arm_disarm=1 WHERE slot=0")) return false;
    }
    Statement s(db, "INSERT OR IGNORE INTO alarm_users_v1(alarm,slot,uid,name,hash,enabled,remaining,revision,api_arm_disarm) "
        "SELECT ?,0,lower(hex(randomblob(16))),'Main',secret,CASE WHEN state=1 THEN 1 ELSE 0 END,-1,1,1 "
        "FROM secrets WHERE uniqueid=? AND length(secret)>0");
    if (!s.valid()) return false;
    s.number(1, alarm); s.text(2, legacyKey(alarm));
    if (s.step() != SQLITE_DONE) return false;
    bool managed = false;
    if (!managementEnabled(alarm, managed)) return false;
    if (!managed) {
        // GET /users does not opt in. Reflect subsequent legacy code0 edits until
        // an explicit successful user mutation activates management atomically.
        Statement refresh(db, "UPDATE alarm_users_v1 SET hash=(SELECT secret FROM secrets WHERE uniqueid=?),"
            "enabled=1,remaining=-1,revision=revision+1 WHERE alarm=? AND slot=0 AND "
            "EXISTS(SELECT 1 FROM secrets WHERE uniqueid=? AND (secret<>hash OR enabled<>1 OR remaining<>-1))");
        if (!refresh.valid()) return false;
        refresh.text(1, legacyKey(alarm)); refresh.number(2, alarm); refresh.text(3, legacyKey(alarm));
        if (refresh.step() != SQLITE_DONE) return false;
        Statement removed(db, "DELETE FROM alarm_users_v1 WHERE alarm=? AND slot=0 AND NOT EXISTS(SELECT 1 FROM secrets WHERE uniqueid=?)");
        if (!removed.valid()) return false;
        removed.number(1, alarm); removed.text(2, legacyKey(alarm));
        if (removed.step() != SQLITE_DONE) return false;
    }
    return true;
}
bool Store::read(int alarm, std::vector<User> &users) {
    users.clear();
    Statement s(db, "SELECT slot,u.uid,name,hash,enabled,remaining,revision,api_arm_disarm,coalesce(p.policy,'') FROM alarm_users_v1 u LEFT JOIN alarm_user_schedules_v1 p ON p.uid=u.uid WHERE alarm=? ORDER BY slot");
    if (!s.valid()) return false;
    s.number(1, alarm);
    int rc;
    while ((rc = s.step()) == SQLITE_ROW) {
        User u; u.slot = int(s.number(0)); u.id = s.text(1); u.name = s.text(2); u.hash = s.text(3);
        u.enabled = s.number(4) != 0; u.remaining = s.number(5); u.revision = s.number(6);
        if (s.number(7) != 0 && s.number(7) != 1) return false;
        u.apiArmDisarm = s.number(7) == 1;
        u.schedule = s.text(8);
        users.push_back(u);
    }
    return rc == SQLITE_DONE;
}
bool Store::list(int alarm, std::vector<User> &users) {
    Transaction t(db);
    return t.active && init(alarm) && read(alarm, users) && t.commit();
}
bool Store::put(int alarm, User &u, const std::string &pin, int64_t revision, std::string &error) {
    // Slot identity, not the editable display name, defines the primary user.
    // Reject restrictions rather than silently correcting the submitted policy.
    if (u.slot == 0 && (!u.enabled || !u.apiArmDisarm || u.remaining != -1 || !u.schedule.empty())) {
        error = "primary_user_protected"; return false;
    }
    error = "invalid_user";
    if (u.slot < 0 || u.slot >= MaxUsers || u.name.empty() || u.name.size() > 64 ||
        std::any_of(u.name.begin(), u.name.end(), [](unsigned char c) { return c < 32 || c == 127; }) ||
        u.remaining < -1 || u.remaining > 1000000 || revision < 0 || (!pin.empty() && !validPin(pin))) return false;
    if (!u.schedule.empty() && (u.schedule.size() > 8192 || !scheduleCheck || scheduleCheck(u.schedule, 0) != 1)) {
        error = "invalid_schedule"; return false;
    }
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
    Statement s(db, "INSERT OR REPLACE INTO alarm_users_v1(alarm,slot,uid,name,hash,enabled,remaining,revision,api_arm_disarm) "
        "VALUES(?,?,coalesce((SELECT uid FROM alarm_users_v1 WHERE alarm=? AND slot=?),lower(hex(randomblob(16)))),?,?,?,?,?,?)");
    if (!s.valid()) return false;
    s.number(1,alarm); s.number(2,u.slot); s.number(3,alarm); s.number(4,u.slot);
    s.text(5,u.name); s.text(6,u.hash); s.number(7,u.enabled); s.number(8,u.remaining); s.number(9,revision+1); s.number(10,u.apiArmDisarm);
    if (s.step()!=SQLITE_DONE) return false;
    Statement policy(db, "INSERT OR REPLACE INTO alarm_user_schedules_v1 SELECT uid,? FROM alarm_users_v1 WHERE alarm=? AND slot=?");
    if (!policy.valid()) return false;
    policy.text(1,u.schedule); policy.number(2,alarm); policy.number(3,u.slot);
    if (policy.step()!=SQLITE_DONE || !mirror(db,alarm,u) || !activate(alarm) || !read(alarm,users) || !t.commit()) return false;
    u = *std::find_if(users.begin(),users.end(),[&](const User &x){return x.slot==u.slot;});
    error.clear(); return true;
}
bool Store::erase(int alarm, int slot, int64_t revision) {
    if (slot <= 0 || slot >= MaxUsers || revision < 1) return false;
    Transaction t(db);
    if (!t.active || !init(alarm)) return false;
    Statement s(db,"DELETE FROM alarm_users_v1 WHERE alarm=? AND slot=? AND revision=?");
    if (!s.valid()) return false;
    s.number(1,alarm); s.number(2,slot); s.number(3,revision);
    if (s.step()!=SQLITE_DONE || sqlite3_changes(db)!=1) return false;
    if (!exec(db,"DELETE FROM alarm_user_schedules_v1 WHERE uid NOT IN (SELECT uid FROM alarm_users_v1)")) return false;
    return activate(alarm) && t.commit();
}
bool Store::restCode(int alarm, const std::string &pin, int64_t now) {
    if (now == -1) now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::vector<User> users;
    if (!list(alarm,users)) return false;
    for (const auto &u:users)
        if (u.apiArmDisarm && u.enabled && u.remaining!=0 && verify(u.hash,pin))
            return u.schedule.empty() || (now > 0 && scheduleCheck && scheduleCheck(u.schedule,now) == 1);
    return false;
}
bool Store::setMainCode(int alarm, const std::string &pin) {
    if (!validPin(pin)) return false;
    std::vector<User> users;
    if (!list(alarm,users)) return false;
    User u; u.slot=0; u.name="Main"; u.apiArmDisarm=true;
    for (const auto &x:users) if (x.slot==0) u=x;
    std::string error;
    return put(alarm,u,pin,u.revision,error); // restricted legacy records require explicit policy repair
}
Result Store::authorize(int alarm,const std::string &source,int endpoint,int sequence,
                        int mode,const std::string &pin,int64_t now,bool alreadyDisarmed) {
    Result result;
    bool managed = false;
    if (!managementEnabled(alarm, managed) || !managed) return result;
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
    int scheduled = 1;
    if (matched.slot >= 0 && !matched.schedule.empty()) {
        scheduled = scheduleCheck ? scheduleCheck(matched.schedule,now) : -1;
        if (scheduled < 0) return result; // policy/clock failure is not_ready, never a close request
    }
    if (matched.slot>=0 && matched.enabled && matched.remaining!=0 && scheduled == 1) {
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
