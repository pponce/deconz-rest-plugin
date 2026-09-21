#include "alarm_user_store.h"
#include <sqlite3.h>
#include <cassert>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <atomic>
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
    User u;u.slot=slot;u.name="User "+std::to_string(slot);u.remaining=uses;std::string err;
    CHECK(s.put(1,u,pin,0,err));return u;
}
int main() {
 try {
    const char *path="alarm-users-test.sqlite";std::remove(path);
    sqlite3 *db=nullptr;CHECK(sqlite3_open(path,&db)==SQLITE_OK);
    sql(db,"CREATE TABLE secrets(uniqueid TEXT PRIMARY KEY,secret TEXT,state INTEGER)");
    const auto original=hash("1357");
    sql(db,"INSERT INTO secrets VALUES('as_1_code0','"+original+"',1)");
    Store store(db,verify,hash);
    auto main=get(store,0);CHECK(main.hash==original);CHECK(main.enabled);CHECK(main.remaining==-1);
    CHECK(store.mainCode(1,"1357"));CHECK(!store.mainCode(1,"0000"));
    auto guest=add(store,1,"0246",5);const auto identity=guest.id;
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
    CHECK(reopened.authorize(1,"device",1,4,0,"0246",108000,true).duplicate);
    guest=get(reopened,1);guest.enabled=false;guest.name="Alex";
    CHECK(reopened.put(1,guest,"",guest.revision,err));CHECK(guest.id==identity);
    CHECK(reopened.authorize(1,"device",1,7,0,"0246",109000,true).response==4);
    guest.enabled=true;CHECK(reopened.put(1,guest,"",guest.revision,err));CHECK(guest.remaining==0);
    CHECK(reopened.authorize(1,"device",1,8,0,"0246",110000,true).response==4);
    guest.remaining=2;CHECK(reopened.put(1,guest,"",guest.revision,err));
    // ARM is allowed but does not consume; REST may only use the main credential.
    CHECK(reopened.authorize(1,"device",1,9,3,"0246",111000,false).response==3);
    CHECK(get(reopened,1).remaining==2);CHECK(!reopened.mainCode(1,"0246"));
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
    // Main PIN changes preserve restrictions and synchronize legacy hash.
    main=get(reopened,0);main.enabled=false;CHECK(reopened.put(1,main,"",main.revision,err));
    CHECK(reopened.setMainCode(1,"1358"));CHECK(!reopened.mainCode(1,"1358"));CHECK(!get(reopened,0).enabled);
    main=get(reopened,0);main.enabled=true;CHECK(reopened.put(1,main,"",main.revision,err));CHECK(reopened.mainCode(1,"1358"));
    CHECK(reopened.erase(1,0,main.revision));CHECK(!reopened.mainCode(1,"1358"));CHECK(list(reopened).size()==8);
    // Deletion must not resurrect the legacy main credential on subsequent reads.
    CHECK(list(reopened).size()==8);
    CHECK(sqlite3_close(db)==SQLITE_OK);std::remove(path);
    std::cout<<"PASS: "<<assertions<<" checks (SQLite persistence, scrypt fixtures, concurrency, policy, retries)\n";
 } catch(const std::exception &e) {std::cerr<<e.what()<<"\n";return 1;}
}
