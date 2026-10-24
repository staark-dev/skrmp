#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "phone_worker.h"
#include "protocol/skrx_phone.h"
#include <sqlite3.h>
#include <filesystem>
#include <future>
#include <set>
#include <cstdio>
#include <cstdlib>
#include <cstring>
using namespace skr;
namespace ph=protocol::phone;
namespace {
int checks=0;
void Check(bool ok,const char* name) { ++checks; if (!ok) { std::printf("FAIL: %s\n",name); std::exit(1); } }
phone_backend::Request Request(int id,const char* owner) {
    phone_backend::Request job; job.identity.playerId=id; job.identity.sessionId=1000+id;
    job.identity.generation=1; job.identity.gateway=true; job.identity.setup=true;
    std::snprintf(job.identity.owner,sizeof(job.identity.owner),"%s",owner);
    job.requestId=1; job.op=1; return job;
}
phone_backend::Response Wait(phone_backend::Worker& worker) {
    phone_backend::Response response; const auto start=GetTickCount64();
    while (!worker.Pop(response) && GetTickCount64()-start<5000) Sleep(1);
    Check(response.size!=0,"worker returns bounded response"); return response;
}
}
int main() {
    Check(!std::strcmp(sqlite3_libversion(),SQLITE_VERSION),"linked SQLite matches inspected header version");
    Check(!std::strcmp(sqlite3_sourceid(),SQLITE_SOURCE_ID),"linked SQLite matches pinned source revision");
    Check(sqlite3_threadsafe()!=0,"SQLite package supports isolated connections across threads");
    std::printf("SQLite runtime: %s\n",sqlite3_libversion());
    const auto dir=std::filesystem::current_path()/"out"/"phone-m58-tests";
    std::filesystem::create_directories(dir);
    const auto path=dir/("worker-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64())+".db");
    phone_backend::Worker worker(path.string());
    const auto start=GetTickCount64();
    while (!worker.State() && GetTickCount64()-start<5000) Sleep(1);
    Check(worker.State()==1,"worker owns successful initialization");
    phone_backend::Database inspector; Check(inspector.Open(path.string()),"independent test SQLite connection");
    auto foreignThread=std::async(std::launch::async,[&]{ return inspector.executeQuery("SELECT 1"); });
    Check(foreignThread.get()==nullptr,"SQLite adapter rejects access from non-owner thread");
    auto invalid=Request(1,"Invalid"); invalid.size=201;
    Check(!worker.Submit(invalid),"oversized queue payload rejected");
    invalid=Request(1,"Invalid"); invalid.identity.sessionId=0;
    Check(!worker.Submit(invalid),"missing session snapshot rejected");
    std::vector<std::future<bool>> producers;
    for (int i=0;i<32;++i) producers.push_back(std::async(std::launch::async,[&,i]{
        return worker.Submit(Request(i+1,("Account"+std::to_string(i)).c_str()));
    }));
    for (auto& producer:producers) Check(producer.get(),"concurrent producer request queued");
    std::set<std::string> numbers; std::string firstOwnerNumber;
    for (int i=0;i<32;++i) {
        const auto result=Wait(worker); ph::PhoneSyncResponse sync;
        Check(result.op==protocol::api::Op::PhoneSync && ph::DecodePhoneSync(result.data,result.size,sync),"allocation has authoritative sync result");
        Check(ph::IsM5PhoneNumber(sync.ownNumber,sync.ownNumberLen),"allocation inside 555 pool");
        numbers.insert(sync.ownNumber);
        if (!std::strcmp(result.identity.owner,"Account0")) firstOwnerNumber=sync.ownNumber;
    }
    Check(numbers.size()==32,"32 concurrent activation requests have unique ownership");
    // Returning a task to the same identity/session reuses one number.
    Check(worker.Submit(Request(1,"Account0")),"same owner reconnect/replay queued");
    auto replay=Wait(worker); ph::PhoneSyncResponse sync;
    Check(ph::DecodePhoneSync(replay.data,replay.size,sync) && sync.ownNumber==firstOwnerNumber,"existing owner is not renumbered");
    std::unique_ptr<phone_backend::Result> locked(inspector.executeQuery("BEGIN EXCLUSIVE"));
    Check(locked!=nullptr,"test holds competing SQLite write lock"); locked.reset();
    // Submit only copies a bounded job. It does not touch this locked DB.
    Check(worker.Submit(Request(90,"Cancelled")),"ingress accepts without acquiring database lock");
    worker.Cancel(90);
    for (int i=0;i<8;++i) worker.Submit(Request(91,"QueueBound"));
    worker.Cancel(91);
    worker.SetEnabled(false);
    Check(!worker.Submit(Request(92,"Disabled")),"service disable rejects new work and cancels leases");
    locked.reset(inspector.executeQuery("ROLLBACK")); Check(locked!=nullptr,"test releases competing lock"); locked.reset();
    worker.SetEnabled(true);
    Check(worker.Submit(Request(93,"AfterCancellation")),"queue resumes after cancellation/disable");
    phone_backend::Response result;
    do { result=Wait(worker); } while (result.identity.playerId!=93);
    Check(result.op==protocol::api::Op::PhoneSync,"worker still processes after cancelled database access");
    worker.Stop();
    std::unique_ptr<phone_backend::Result> rows(inspector.executeQuery("SELECT COUNT(*) FROM phone_numbers WHERE nick IN ('Cancelled','QueueBound','Disabled')"));
    Check(rows && rows->getFieldInt(0)==0,"cancelled/disabled queued work did not allocate"); rows.reset();
    rows.reset(inspector.executeQuery("SELECT COUNT(*),COUNT(DISTINCT number) FROM phone_numbers"));
    Check(rows && rows->getFieldInt(0)==33 && rows->getFieldInt(1)==33,"one durable number per completed activation"); rows.reset();
    // Schema still accepts the old activated identity and preserves data across migration.
    rows.reset(inspector.executeQuery("INSERT INTO phone_numbers VALUES('Legacy','1000000',1)"));
    Check(rows!=nullptr,"legacy ownership remains valid"); rows.reset(); inspector.Close();
    phone_backend::Store reopened; Check(reopened.Initialize(path.string()),"startup migration reopens existing DB");
    auto legacy=Request(94,"Legacy"); legacy.identity.lease=std::make_shared<std::atomic<bool>>(true);
    const auto legacyResult=reopened.Process(legacy);
    Check(legacyResult.size()==1 && ph::DecodePhoneSync(legacyResult[0].data,legacyResult[0].size,sync) && !std::strcmp(sync.ownNumber,"1000000"),"legacy number remains unchanged");
    legacy.op=static_cast<uint8_t>(ph::ClientOp::ProfileGet);
    const auto profileResult=reopened.Process(legacy); ph::Profile profile;
    Check(profileResult.size()==1 && ph::DecodeProfile(profileResult[0].data,profileResult[0].size,profile) && profile.stage==4,"legacy activated phone is grandfathered");
    reopened.Close();
    std::filesystem::remove(path);
    std::printf("phone_worker: %d checks passed\n",checks); return 0;
}
