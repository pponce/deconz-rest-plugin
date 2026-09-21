#ifndef ALARM_USER_STORE_H
#define ALARM_USER_STORE_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>
struct sqlite3;

namespace AlarmUsers {
constexpr int MaxUsers = 9;
constexpr int64_t DuplicateWindowMs = 10000;
struct User {
    int slot = -1;
    std::string id, name, hash;
    bool enabled = true;
    int64_t remaining = -1; // -1 unlimited; zero exhausted
    int64_t revision = 0;
};
struct Result {
    bool ok = false; // false = storage error, NEVER invalid code
    bool duplicate = false;
    int response = 5; // IAS not_ready
    User user;
    std::string eventId;
};
using Verify = std::function<bool(const std::string &, const std::string &)>;
using Hash = std::function<std::string(const std::string &)>;

// Caller supplies the gateway DB connection; every mutation commits before success.
// Main code (slot 0) remains mirrored in secrets for legacy REST compatibility.
class Store {
public:
    Store(sqlite3 *db, Verify verify, Hash hash);
    // Read-only opt-in check. Failure is not permission to fall back to legacy.
    bool managementEnabled(int alarm, bool &enabled);
    bool list(int alarm, std::vector<User> &users);
    // expectedRevision=0 creates; >0 edits. Empty pin preserves existing hash.
    bool put(int alarm, User &user, const std::string &pin, int64_t expectedRevision,
             std::string &error);
    bool erase(int alarm, int slot, int64_t expectedRevision);
    bool mainCode(int alarm, const std::string &pin); // REST; never consumes uses
    bool setMainCode(int alarm, const std::string &pin); // legacy config/code0
    Result authorize(int alarm, const std::string &source, int endpoint, int sequence,
                     int mode, const std::string &pin, int64_t nowMs, bool alreadyDisarmed);
private:
    sqlite3 *db;
    Verify verify;
    Hash hash;
    bool init(int alarm);
    bool read(int alarm, std::vector<User> &users);
    bool activate(int alarm);
};
}
#endif
