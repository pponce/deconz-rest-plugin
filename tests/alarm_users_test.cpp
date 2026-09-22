#include "alarm_user_store.h"
#include <sqlite3.h>
#include <cassert>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <atomic>
#include <algorithm>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <sstream>
#include <iomanip>
using namespace AlarmUsers;
static int assertions = 0;
#define CHECK(x) do { ++assertions; if (!(x)) throw std::runtime_error("line " + std::to_string(__LINE__) + ": " #x); } while (0)
// Real scrypt for fixtures; production uses the gateway's existing PHC helper.
static std::string derive(const std::string &pin,const std::string &salt) {
    unsigned char bytes[32];
    if (!EVP_PBE_scrypt(pin.data(),pin.size(),reinterpret_cast<const unsigned char*>(salt.data()),salt.size(),1024,8,1,0,bytes,sizeof bytes)) return "";
    std::ostringstream out;
    for(auto b:bytes) out<<std::hex<<std::setw(2)<<std::setfill('0')<<int(b);
    return out.str();
}
static std::string hash(const std::string &pin) {
    unsigned char salt[8]; CHECK(RAND_bytes(salt,sizeof salt)==1);
    std::ostringstream s;for(auto c:salt)s<<std::hex<<std::setw(2)<<std::setfill('0')<<int(c);
    return s.str()+":"+derive(pin,s.str());
}
static bool verify(const std::string &value,const std::string &pin) {
    auto i=value.find(':'); return i!=std::string::npos && value.substr(i+1)==derive(pin,value.substr(0,i));
}
static void sql(sqlite3 *db,const std::string &s) { CHECK(sqlite3_exec(db,s.c_str(),nullptr,nullptr,nullptr)==SQLITE_OK); }
static std::vector<User> list(Store &s) { std::vector<User> u; CHECK(s.list(1,u));return u; }
static User get(Store &s,int slot) { for(auto u:list(s))if(u.slot==slot)return u;throw std::runtime_error("missing"); }
static User add(Store &s,int slot,const std::string &pin,int64_t uses=-1) {
    User u;u.slot=slot;u.name="User "+std::to_string(slot);u.remaining=uses;u.apiArmDisarm=slot==0;std::string err;
    CHECK(s.put(1,u,pin,0,err));return u;
}
static void lockoutPersistenceTests() {
    const char *path="/tmp/alarm-lockout-fixture.sqlite";std::remove(path);
    sqlite3 *db=nullptr;CHECK(sqlite3_open(path,&db)==SQLITE_OK);
    sql(db,"CREATE TABLE secrets(uniqueid TEXT PRIMARY KEY,secret TEXT,state INTEGER)");
    Store s(db,verify,hash);add(s,0,"1357");LockoutPolicy p;p.enabled=true;p.threshold=1;std::string error;
    CHECK(s.configureLockout(1,p,0,error));
    CHECK(s.authorize(1,"pad",1,1,0,"9999",100000,false).locked);
    CHECK(sqlite3_close(db)==SQLITE_OK);CHECK(sqlite3_open(path,&db)==SQLITE_OK);
    Store reopened(db,verify,hash);
    auto r=reopened.authorize(1,"pad",1,2,0,"1357",100001,false);
    CHECK(r.ok&&r.locked&&r.lockedUntil==160000);
    sql(db,"PRAGMA query_only=ON");
    CHECK(!reopened.resetLockout(1));CHECK(!reopened.authorize(1,"pad",1,3,0,"1357",100002,false).ok);
    sql(db,"PRAGMA query_only=OFF");
    CHECK(reopened.resetLockout(1));CHECK(reopened.authorize(1,"pad",1,4,0,"1357",100003,false).response==0);
    CHECK(sqlite3_close(db)==SQLITE_OK);std::remove(path);
}
static void lockoutTests() {
    sqlite3 *db=nullptr;CHECK(sqlite3_open(":memory:",&db)==SQLITE_OK);
    sql(db,"CREATE TABLE secrets(uniqueid TEXT PRIMARY KEY,secret TEXT,state INTEGER)");
    Store s(db,verify,hash);LockoutPolicy p;std::vector<LockoutState> states;std::string error;
    CHECK(s.lockout(1,p,states) && !p.enabled && states.empty());
    bool managed=true;CHECK(s.managementEnabled(1,managed)&&!managed);
    p.enabled=true;CHECK(!s.configureLockout(1,p,0,error)&&error=="managed_users_required");
    add(s,0,"1357");add(s,1,"2468",5);
    CHECK(s.lockout(1,p,states)&&!p.enabled);
    for(int i=0;i<5;++i)CHECK(s.authorize(1,"pad",1,i,0,"9999",100000+i,false).response==4);
    CHECK(s.authorize(1,"pad",1,6,0,"1357",100006,false).response==0);
    p.enabled=true;p.durations[0]=60;p.durations[1]=1200;p.durations[2]=3600;
    CHECK(s.configureLockout(1,p,0,error));
    CHECK(!s.configureLockout(1,p,0,error)&&error=="revision_conflict");
    auto invalid=p;invalid.durations[2]=3601;CHECK(!s.configureLockout(1,invalid,p.revision,error));
    invalid=p;invalid.durations[1]=1;CHECK(!s.configureLockout(1,invalid,p.revision,error));
    int seq=10;int64_t now=200000;
    auto enter=[&](const char *pin) {return s.authorize(1,"pad",1,seq++%256,0,pin,now++,true);};
    auto r=enter("9999");CHECK(r.ok&&!r.locked&&r.response==4);
    auto dup=s.authorize(1,"pad",1,10,0,"9999",now,true);CHECK(dup.duplicate&&!dup.locked);
    CHECK(!enter("9999").locked);r=enter("9999");CHECK(r.locked&&r.lockoutLevel==1&&r.lockedUntil==260002);
    const auto until=r.lockedUntil;
    for(int mode=0;mode<4;++mode) {
        r=s.authorize(1,"pad",1,seq++,mode,"1357",now++,true);
        CHECK(r.ok&&r.response==4&&r.locked&&r.lockedUntil==until&&r.user.slot==-1);
    }
    r=enter("2468");CHECK(r.locked&&get(s,1).remaining==5);
    CHECK(s.restCode(1,"1357",now)); // Another authenticated route remains available.
    CHECK(s.authorize(1,"other",1,1,0,"1357",now,true).response==6);
    CHECK(s.authorize(1,"pad",2,1,0,"1357",now,true).response==6);
    Store reopened(db,verify,hash);CHECK(reopened.lockout(1,p,states));
    CHECK(reopened.authorize(1,"pad",1,seq++,0,"1357",now,true).locked);
    for(int level=2;level<=4;++level) {
        CHECK(s.lockout(1,p,states));
        for(const auto &st:states)if(st.source=="pad"&&st.endpoint==1)now=st.until;
        CHECK(!enter("9999").locked);CHECK(!enter("9999").locked);r=enter("9999");
        CHECK(r.locked&&r.lockoutLevel==std::min(level,3));
        CHECK(r.lockedUntil==now-1+int64_t(p.durations[std::min(level,3)-1])*1000);
    }
    CHECK(s.resetLockout(1));CHECK(enter("1357").response==6);
    CHECK(s.lockout(1,p,states)&&p.enabled);
    CHECK(!enter("9999").locked);now+=60001;CHECK(!enter("9999").locked);CHECK(!enter("9999").locked);
    CHECK(enter("1357").response==6);CHECK(!enter("9999").locked); // success clears failures
    auto guest=get(s,1);guest.enabled=false;CHECK(s.put(1,guest,"",guest.revision,error));
    CHECK(enter("2468").response==4);CHECK(!enter("9999").locked);CHECK(!enter("9999").locked);
    r=enter("9999");CHECK(r.locked&&r.lockoutLevel==1);
    now+=int64_t(p.resetSeconds)*1000+1;
    CHECK(!enter("9999").locked);CHECK(!enter("9999").locked);CHECK(enter("9999").lockoutLevel==1);
    CHECK(s.configureLockout(1,p,p.revision,error)); // preserves active block
    CHECK(enter("1357").locked);
    p.enabled=false;CHECK(s.configureLockout(1,p,p.revision,error));CHECK(enter("1357").response==6);
    CHECK(s.lockout(1,p,states)&&states.empty());
    p.enabled=true;CHECK(s.configureLockout(1,p,p.revision,error));
    CHECK(!enter("9999").locked);CHECK(!enter("9999").locked);CHECK(enter("9999").locked);
    CHECK(!s.authorize(1,"pad",1,seq++,0,"1357",now-10000,true).ok); // backward clock fails closed
    CHECK(s.resetLockout(1));CHECK(enter("1357").response==6);
    CHECK(sqlite3_close(db)==SQLITE_OK);
}
static void primaryProtectionTests() {
    sqlite3 *db=nullptr; CHECK(sqlite3_open(":memory:",&db)==SQLITE_OK);
    sql(db,"CREATE TABLE secrets(uniqueid TEXT PRIMARY KEY,secret TEXT,state INTEGER)");
    Store store(db,verify,hash,[](const std::string &,int64_t){ return 1; });
    auto primary=add(store,0,"1357");std::string err;
    for (int restriction=0; restriction<5; ++restriction) {
        auto changed=primary;
        if (restriction==0) changed.enabled=false;
        if (restriction==1) changed.apiArmDisarm=false;
        if (restriction==2) changed.remaining=0;
        if (restriction==3) changed.remaining=20;
        if (restriction==4) changed.schedule="valid-policy";
        CHECK(!store.put(1,changed,"9876",primary.revision,err));
        CHECK(err=="primary_user_protected");
        auto current=get(store,0);
        CHECK(current.id==primary.id && current.hash==primary.hash && current.revision==primary.revision);
        CHECK(current.enabled && current.apiArmDisarm && current.remaining==-1 && current.schedule.empty());
        CHECK(store.restCode(1,"1357") && !store.restCode(1,"9876"));
    }
    CHECK(!store.erase(1,0,primary.revision));
    primary.name="Renamed owner";CHECK(store.put(1,primary,"2468",primary.revision,err));
    CHECK(store.restCode(1,"2468") && !store.restCode(1,"1357"));
    for(int mode=0;mode<=3;++mode) {
        auto result=store.authorize(1,"pad",1,mode,mode,"2468",100000+mode*1000,false);
        CHECK(result.ok && result.response==mode && result.user.remaining==-1);
    }
    primary=get(store,0);
    CHECK(!store.put(1,primary,"1234",primary.revision-1,err) && err=="revision_conflict");
    CHECK(!store.setMainCode(1,"")); // Empty preserves an existing credential, never deletes it.
    CHECK(store.restCode(1,"2468"));
    CHECK(sqlite3_close(db)==SQLITE_OK);
}
static void schedulePolicyTests() {
    sqlite3 *db=nullptr; CHECK(sqlite3_open(":memory:",&db)==SQLITE_OK);
    sql(db,"CREATE TABLE secrets(uniqueid TEXT PRIMARY KEY,secret TEXT,state INTEGER)");
    // Inject deterministic schedule evaluation to test transaction behavior independently of Qt.
    auto check=[](const std::string &policy,int64_t now) {
        if(policy!="window")return -1;
        if(now==0)return 1;
        return now>=100000 && now<120000 ? 1 : 0;
    };
    Store store(db,verify,hash,check); auto user=add(store,1,"2468",3);std::string err;
    user.schedule="window";user.apiArmDisarm=true;CHECK(store.put(1,user,"",user.revision,err));
    CHECK(!store.restCode(1,"2468",99999));CHECK(store.restCode(1,"2468",100000));
    CHECK(store.authorize(1,"pad",1,1,0,"2468",99999,true).response==4);
    CHECK(get(store,1).remaining==3);
    auto accepted=store.authorize(1,"pad",1,2,0,"2468",119000,true);
    CHECK(accepted.response==6 && accepted.user.remaining==2);
    // A receipt crossing expiry acknowledges only the old request; no second use or event.
    auto duplicate=store.authorize(1,"pad",1,2,0,"2468",121000,true);
    CHECK(duplicate.ok && duplicate.duplicate && duplicate.user.remaining==2);
    CHECK(store.authorize(1,"pad",1,3,0,"2468",121001,true).response==4);
    CHECK(!store.restCode(1,"2468",120000));
    user=get(store,1);user.enabled=false;CHECK(store.put(1,user,"",user.revision,err));
    CHECK(!store.restCode(1,"2468",110000));
    user.enabled=true;CHECK(store.put(1,user,"",user.revision,err));CHECK(user.remaining==2);
    CHECK(!store.restCode(1,"2468",130000));
    Store restarted(db,verify,hash,check);CHECK(get(restarted,1).schedule=="window");
    CHECK(!restarted.restCode(1,"2468",130000));
    Store noEvaluator(db,verify,hash);CHECK(!noEvaluator.restCode(1,"2468",110000));
    CHECK(!noEvaluator.authorize(1,"pad",1,4,0,"2468",110000,true).ok);
    user=get(store,1);user.schedule="bad";CHECK(!store.put(1,user,"",user.revision,err));
    CHECK(err=="invalid_schedule" && get(store,1).schedule=="window");
    sql(db,"CREATE TRIGGER deny_schedule BEFORE INSERT ON alarm_user_schedules_v1 BEGIN SELECT RAISE(ABORT,'test'); END");
    user=get(store,1);user.name="changed";CHECK(!store.put(1,user,"",user.revision,err));CHECK(get(store,1).name!="changed");
    sql(db,"DROP TRIGGER deny_schedule");
    user=get(store,1);CHECK(store.erase(1,1,user.revision));
    auto replacement=add(store,1,"2468");CHECK(replacement.schedule.empty());
    // Main can never acquire a schedule, including around a legacy PIN update.
    auto main=add(store,0,"1357");main.schedule="window";
    CHECK(!store.put(1,main,"",main.revision,err) && err=="primary_user_protected");
    CHECK(store.restCode(1,"1357",130000));CHECK(store.setMainCode(1,"1358"));
    CHECK(get(store,0).schedule.empty() && store.restCode(1,"1358",130000));
    CHECK(sqlite3_close(db)==SQLITE_OK);
}
static void apiPermissionTests() {
    const char *path="alarm-api-permission-test.sqlite"; std::remove(path);
    sqlite3 *db=nullptr; CHECK(sqlite3_open(path,&db)==SQLITE_OK);
    // Exact pre-permission table layout: migration must work for already-managed alarms.
    sql(db,"CREATE TABLE secrets(uniqueid TEXT PRIMARY KEY,secret TEXT,state INTEGER)");
    sql(db,"CREATE TABLE alarm_user_management_v1(alarm INTEGER PRIMARY KEY)");
    sql(db,"CREATE TABLE alarm_users_v1(alarm INTEGER NOT NULL,slot INTEGER NOT NULL,uid TEXT NOT NULL UNIQUE,name TEXT NOT NULL,hash TEXT NOT NULL,enabled INTEGER NOT NULL,remaining INTEGER NOT NULL,revision INTEGER NOT NULL,PRIMARY KEY(alarm,slot))");
    const auto mainHash=hash("1357"),guestHash=hash("2468");
    sql(db,"INSERT INTO secrets VALUES('as_1_code0','"+mainHash+"',1)");
    sql(db,"INSERT INTO alarm_users_v1 VALUES(1,0,'main-one','Renamed main','"+mainHash+"',1,-1,7)");
    sql(db,"INSERT INTO alarm_users_v1 VALUES(1,1,'guest-one','Guest','"+guestHash+"',1,3,4)");
    sql(db,"INSERT INTO alarm_users_v1 VALUES(2,0,'main-two','Second main','"+mainHash+"',1,-1,9)");
    sql(db,"INSERT INTO alarm_user_management_v1 VALUES(1),(2)");
    Store s(db,verify,hash);
    auto main=get(s,0), guest=get(s,1); std::string err;
    CHECK(main.apiArmDisarm && main.name=="Renamed main" && main.revision==7 && main.hash==mainHash);
    CHECK(!guest.apiArmDisarm && guest.remaining==3 && guest.revision==4);
    std::vector<User> second; CHECK(s.list(2,second));
    CHECK(second.size()==1 && second[0].apiArmDisarm && second[0].enabled && second[0].remaining==-1);
    CHECK(s.restCode(1,"1357") && !s.restCode(1,"2468") && s.restCode(2,"1357"));
    // Default-off API permission does not affect physical arming/disarming.
    CHECK(s.authorize(1,"keypad",1,1,3,"2468",100000,false).response==3);
    CHECK(s.authorize(1,"keypad",1,2,0,"2468",101000,true).response==6);
    guest=get(s,1); CHECK(guest.remaining==2);
    guest.apiArmDisarm=true; CHECK(s.put(1,guest,"",guest.revision,err));
    const auto revision=guest.revision;
    CHECK(s.restCode(1,"2468") && s.restCode(1,"1357")); // multiple API users
    CHECK(!s.restCode(1,"9999") && !s.restCode(2,"2468")); // alarm-scoped
    CHECK(get(s,1).remaining==2 && get(s,1).revision==revision); // REST does not consume
    guest.name="Renamed guest"; CHECK(s.put(1,guest,"",guest.revision,err));
    CHECK(guest.apiArmDisarm && guest.id=="guest-one" && s.restCode(1,"2468"));
    main.apiArmDisarm=false; CHECK(!s.put(1,main,"",main.revision,err) && err=="primary_user_protected");
    CHECK(s.restCode(1,"1357"));
    CHECK(s.authorize(1,"keypad",1,3,0,"1357",102000,true).response==6);
    CHECK(s.setMainCode(1,"1358")); CHECK(s.restCode(1,"1358"));
    CHECK(get(s,0).apiArmDisarm);
    CHECK(sqlite3_close(db)==SQLITE_OK); db=nullptr; CHECK(sqlite3_open(path,&db)==SQLITE_OK);
    Store reopened(db,verify,hash);
    CHECK(get(reopened,0).apiArmDisarm && get(reopened,1).apiArmDisarm);
    CHECK(reopened.restCode(1,"1358") && reopened.restCode(1,"2468"));
    guest=get(reopened,1); guest.enabled=false;
    CHECK(reopened.put(1,guest,"",guest.revision,err)); CHECK(!reopened.restCode(1,"2468"));
    guest.enabled=true; guest.remaining=0;
    CHECK(reopened.put(1,guest,"",guest.revision,err)); CHECK(!reopened.restCode(1,"2468"));
    guest.remaining=1; CHECK(reopened.put(1,guest,"2469",guest.revision,err));
    CHECK(!reopened.restCode(1,"2468") && reopened.restCode(1,"2469"));
    // Failed permission mutation must not change the PIN, policy, identity or revision.
    const auto before=guest; guest.apiArmDisarm=false;
    sql(db,"CREATE TRIGGER deny_policy BEFORE INSERT ON alarm_users_v1 BEGIN SELECT RAISE(ABORT,'fixture'); END");
    CHECK(!reopened.put(1,guest,"",guest.revision,err));
    CHECK(!reopened.restCode(1,"2469")); // storage failure denies authentication
    sql(db,"DROP TRIGGER deny_policy");
    CHECK(get(reopened,1).apiArmDisarm && get(reopened,1).revision==before.revision);
    CHECK(reopened.restCode(1,"2469"));
    guest=get(reopened,1); guest.apiArmDisarm=false;
    CHECK(!reopened.put(1,guest,"",guest.revision-1,err) && err=="revision_conflict");
    CHECK(get(reopened,1).apiArmDisarm);
    CHECK(reopened.erase(1,1,guest.revision)); CHECK(!reopened.restCode(1,"2469"));
    auto replacement=add(reopened,1,"2469");
    CHECK(!replacement.apiArmDisarm && replacement.id!=before.id && !reopened.restCode(1,"2469"));
    // A database policy read error must not silently authenticate via Main.
    sql(db,"ALTER TABLE alarm_users_v1 RENAME COLUMN api_arm_disarm TO damaged_policy");
    sql(db,"CREATE TRIGGER deny_migration BEFORE UPDATE ON alarm_users_v1 BEGIN SELECT RAISE(ABORT,'fixture'); END");
    CHECK(!reopened.restCode(1,"1358"));
    CHECK(sqlite3_close(db)==SQLITE_OK); std::remove(path);
}
static void rejectedReceiptTests() {
    sqlite3 *db=nullptr; CHECK(sqlite3_open(":memory:",&db)==SQLITE_OK);
    sql(db,"CREATE TABLE secrets(uniqueid TEXT PRIMARY KEY,secret TEXT,state INTEGER)");
    Store store(db,verify,hash);
    bool managed=true; CHECK(store.managementEnabled(1,managed) && !managed);
    auto legacy=store.authorize(1,"pad",1,1,0,"9999",100000,true);
    CHECK(!legacy.ok && legacy.eventId.empty());
    auto primary=add(store,0,"1357");
    const auto before=get(store,0);
    auto denied=store.authorize(1,"pad",1,1,0,"9999",100000,true);
    CHECK(denied.ok && denied.response==4 && !denied.duplicate && !denied.eventId.empty());
    CHECK(denied.user.slot==-1 && denied.user.id.empty());
    auto retry=store.authorize(1,"pad",1,1,0,"9999",100001,true);
    CHECK(retry.ok && retry.duplicate && retry.response==4 && retry.eventId==denied.eventId);
    auto next=store.authorize(1,"pad",1,2,0,"9999",100002,true);
    CHECK(next.ok && !next.duplicate && next.response==4 && next.eventId!=denied.eventId);
    auto good=store.authorize(1,"pad",1,3,0,"1357",100003,true);
    CHECK(good.ok && good.response==6 && good.eventId!=next.eventId);
    CHECK(get(store,0).remaining==-1);
    CHECK(get(store,0).revision==before.revision+1);
    Store reopened(db,verify,hash);
    auto persisted=reopened.authorize(1,"pad",1,2,0,"9999",100004,true);
    CHECK(persisted.ok && persisted.duplicate && persisted.eventId==next.eventId);
    CHECK(sqlite3_close(db)==SQLITE_OK);
}

int main() {
 try {
    const char *path="alarm-users-test.sqlite";std::remove(path);
    sqlite3 *db=nullptr;CHECK(sqlite3_open(path,&db)==SQLITE_OK);
    sql(db,"CREATE TABLE secrets(uniqueid TEXT PRIMARY KEY,secret TEXT,state INTEGER)");
    const auto original=hash("1357");
    sql(db,"INSERT INTO secrets VALUES('as_1_code0','"+original+"',1)");
    Store store(db,verify,hash);
    bool managed=true;
    Store unavailable(nullptr,verify,hash);CHECK(!unavailable.managementEnabled(1,managed));
    CHECK(store.managementEnabled(1,managed) && !managed);
    CHECK(!store.authorize(1,"device",1,1,0,"1357",100000,true).ok);
    sqlite3_stmt *schema=nullptr;
    CHECK(sqlite3_prepare_v2(db,"SELECT count(*) FROM sqlite_master WHERE name LIKE 'alarm_user%'",-1,&schema,nullptr)==SQLITE_OK);
    CHECK(sqlite3_step(schema)==SQLITE_ROW && sqlite3_column_int(schema,0)==0);
    sqlite3_finalize(schema); // Default checks neither migrate nor create access receipts.
    auto main=get(store,0);CHECK(main.hash==original);CHECK(main.enabled);CHECK(main.remaining==-1);
    CHECK(store.managementEnabled(1,managed) && !managed); // GET/list is not opt-in.
    const auto changed=hash("legacy-format-code");
    sql(db,"UPDATE secrets SET secret='"+changed+"' WHERE uniqueid='as_1_code0'");
    CHECK(get(store,0).hash==changed); // Legacy edits after GET stay authoritative.
    sql(db,"UPDATE secrets SET secret='"+original+"' WHERE uniqueid='as_1_code0'");
    // A failed explicit mutation must not activate management or retain its user.
    sql(db,"CREATE TRIGGER deny_optin BEFORE INSERT ON alarm_user_management_v1 BEGIN SELECT RAISE(ABORT,'test'); END");
    User failed;failed.slot=1;failed.name="Failed";std::string failure;
    CHECK(!store.put(1,failed,"0246",0,failure));
    CHECK(store.managementEnabled(1,managed) && !managed);CHECK(list(store).size()==1);
    sql(db,"DROP TRIGGER deny_optin");
    CHECK(store.restCode(1,"1357"));CHECK(!store.restCode(1,"0000"));
    auto guest=add(store,1,"0246",5);const auto identity=guest.id;
    CHECK(store.managementEnabled(1,managed) && managed);
    CHECK(store.managementEnabled(2,managed) && !managed); // Independent opt-in per alarm.
    std::string err;
    auto dupe=guest;dupe.slot=2;CHECK(!store.put(1,dupe,"0246",0,err));CHECK(err=="pin_already_assigned");
    for(int i=0;i<5;i++) {
        auto r=store.authorize(1,"device",1,i,0,"0246",100000+i*1000,true);
        CHECK(r.ok && !r.duplicate && r.response==6);CHECK(r.user.id==identity);CHECK(r.user.remaining==4-i);
        auto retry=store.authorize(1,"device",1,i,0,"0246",100100+i*1000,true);
        CHECK(retry.ok && retry.duplicate && retry.eventId==r.eventId);CHECK(get(store,1).remaining==4-i);
    }
    auto sixth=store.authorize(1,"device",1,6,0,"0246",106000,true);
    CHECK(sixth.ok && sixth.response==4 && sixth.user.id.empty());
    auto finalRetry=store.authorize(1,"device",1,4,0,"0246",107000,true);
    CHECK(finalRetry.ok && finalRetry.duplicate && finalRetry.response==6);
    // Persisted receipt/counter survive close/reopen, including final-use retry.
    CHECK(sqlite3_close(db)==SQLITE_OK);db=nullptr;CHECK(sqlite3_open(path,&db)==SQLITE_OK);
    Store reopened(db,verify,hash);CHECK(get(reopened,1).remaining==0);
    CHECK(reopened.managementEnabled(1,managed) && managed);
    CHECK(reopened.authorize(1,"device",1,4,0,"0246",108000,true).duplicate);
    guest=get(reopened,1);guest.enabled=false;guest.name="Alex";
    CHECK(reopened.put(1,guest,"",guest.revision,err));CHECK(guest.id==identity);
    CHECK(reopened.authorize(1,"device",1,7,0,"0246",109000,true).response==4);
    guest.enabled=true;CHECK(reopened.put(1,guest,"",guest.revision,err));CHECK(guest.remaining==0);
    CHECK(reopened.authorize(1,"device",1,8,0,"0246",110000,true).response==4);
    guest.remaining=2;CHECK(reopened.put(1,guest,"",guest.revision,err));
    // ARM is allowed but does not consume; REST may only use the main credential.
    CHECK(reopened.authorize(1,"device",1,9,3,"0246",111000,false).response==3);
    CHECK(get(reopened,1).remaining==2);CHECK(!reopened.restCode(1,"0246"));
    auto oldRevision=guest.revision;CHECK(!reopened.put(1,guest,"",oldRevision,err));CHECK(err=="revision_conflict");
    // Wrong PIN never decrements; disabled PIN cannot be reassigned elsewhere.
    CHECK(reopened.authorize(1,"device",1,10,0,"9999",112000,false).response==4);
    guest=get(reopened,1);guest.enabled=false;CHECK(reopened.put(1,guest,"",guest.revision,err));
    CHECK(!reopened.put(1,dupe,"0246",0,err));
    guest.enabled=true;CHECK(reopened.put(1,guest,"2468",guest.revision,err));CHECK(guest.id==identity);
    CHECK(reopened.authorize(1,"device",1,11,0,"0246",113000,false).response==4);
    auto r=reopened.authorize(1,"device",1,12,0,"2468",114000,false);CHECK(r.response==0 && r.user.remaining==1);
    // Same sequence with changed mode is uncertain, not a second grant.
    CHECK(!reopened.authorize(1,"device",1,12,3,"2468",114100,false).ok);
    CHECK(!reopened.authorize(1,"device",1,12,0,"2468",113999,false).ok);
    // Sequence wrap/reuse after the bounded retry window is a new request.
    CHECK(reopened.authorize(1,"device",1,12,0,"2468",125000,false).user.remaining==0);
    for(int i=2;i<9;i++)add(reopened,i,std::to_string(3000+i));
    CHECK(list(reopened).size()==9);dupe.slot=9;CHECK(!reopened.put(1,dupe,"4000",0,err));
    guest=get(reopened,1);CHECK(!reopened.erase(1,1,guest.revision-1));CHECK(reopened.erase(1,1,guest.revision));
    auto recreated=add(reopened,1,"2468",1);CHECK(recreated.id!=identity);
    // Storage failure rolls back decrement and receipt together.
    sql(db,"CREATE TRIGGER deny_receipt BEFORE INSERT ON alarm_user_requests_v1 BEGIN SELECT RAISE(ABORT,'test'); END");
    CHECK(!reopened.authorize(1,"device",1,20,0,"2468",200000,true).ok);CHECK(get(reopened,1).remaining==1);
    sql(db,"DROP TRIGGER deny_receipt");
    // Two connections race for one remaining use: only one may succeed.
    sqlite3 *other=nullptr;CHECK(sqlite3_open(path,&other)==SQLITE_OK);
    sqlite3_busy_timeout(db,5000);sqlite3_busy_timeout(other,5000);
    Store second(other,verify,hash);Result a,b;
    std::thread t1([&]{a=reopened.authorize(1,"device",1,21,0,"2468",201000,true);});
    std::thread t2([&]{b=second.authorize(1,"device2",1,22,0,"2468",201000,true);});
    t1.join();t2.join();CHECK(a.ok && b.ok);CHECK((a.response==6)+(b.response==6)==1);CHECK(get(reopened,1).remaining==0);
    CHECK(sqlite3_close(other)==SQLITE_OK);
    // Main restrictions/deletion are rejected; rotation still mirrors the legacy hash.
    main=get(reopened,0);const auto mainBefore=main;main.enabled=false;
    CHECK(!reopened.put(1,main,"",main.revision,err) && err=="primary_user_protected");
    CHECK(get(reopened,0).revision==mainBefore.revision && get(reopened,0).hash==mainBefore.hash);
    CHECK(reopened.setMainCode(1,"1358"));CHECK(reopened.restCode(1,"1358"));CHECK(get(reopened,0).enabled);
    main=get(reopened,0);main.name="Owner";CHECK(reopened.put(1,main,"",main.revision,err));
    CHECK(main.id==mainBefore.id && main.slot==0);
    CHECK(!reopened.erase(1,0,main.revision));CHECK(reopened.restCode(1,"1358"));CHECK(list(reopened).size()==9);
    CHECK(list(reopened).size()==9);
    CHECK(reopened.managementEnabled(1,managed) && managed); // Never silently fall back.
    CHECK(sqlite3_close(db)==SQLITE_OK);std::remove(path);
    apiPermissionTests();
    schedulePolicyTests();
    primaryProtectionTests();
    rejectedReceiptTests();
    lockoutTests();
    lockoutPersistenceTests();
    std::cout<<"PASS: "<<assertions<<" checks (SQLite persistence, scrypt fixtures, concurrency, policy, retries)\n";
 } catch(const std::exception &e) {std::cerr<<e.what()<<"\n";return 1;}
}

