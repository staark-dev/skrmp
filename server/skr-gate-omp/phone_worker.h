#pragma once
#include "protocol/phone_protocol.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

struct sqlite3;
namespace skr::phone_backend {
struct Identity {
    int playerId=-1; uint64_t generation=0; uint32_t sessionId=0;
    bool gateway=false,setup=false; char owner[25]={};
    std::shared_ptr<std::atomic<bool>> lease;
};
struct Request {
    Identity identity; uint32_t requestId=0; uint8_t op=0;
    size_t size=0; uint8_t data[protocol::api::kMaxClientRequestPayload]={};
};
struct Response {
    Identity identity; // exact requester, unless ownerPush is true
    bool ownerPush=false; char recipient[25]={};
    protocol::api::Op op=protocol::api::Op::PhoneError;
    size_t size=0; uint8_t data[protocol::api::kMaxCallPayload]={};
};
// Project-owned, bounded SQLite result adapter; not an invented open.mp API.
struct Result {
    std::vector<std::vector<std::string>> rows; size_t row=0,bytes=0;
    size_t getRowCount() const { return rows.size(); }
    bool selectNextRow() { if (row+1>=rows.size()) return false; ++row; return true; }
    std::string_view getFieldString(size_t i) const { return row<rows.size() && i<rows[row].size()?std::string_view(rows[row][i]):std::string_view(); }
    long getFieldInt(size_t i) const;
};
class Database {
public:
    Database()=default;
    ~Database() { Close(); }
    Database(const Database&)=delete; Database& operator=(const Database&)=delete;
    bool Open(const std::string& path);
    void Close();
    void FinishRequest() noexcept;
    Result* executeQuery(std::string_view sql);
    void SetLease(std::shared_ptr<std::atomic<bool>> lease) { lease_=std::move(lease); }
private:
    sqlite3* db_=nullptr;
    std::thread::id ownerThread_;
    std::shared_ptr<std::atomic<bool>> lease_;
};
class Store {
public:
    bool Initialize(const std::string& path);
    void Close() { database_.Close(); }
    std::vector<Response> Process(const Request& request);
private:
    bool LookupOrAllocate(const char* nick,char* out,bool allow);
    void PushSms(const char* sender,const char* recipient,const char* text,size_t len,uint32_t ts,uint32_t id,std::vector<Response>& output);
    Database database_;
    Database* phoneDb_=&database_;
};
class Worker {
public:
    static constexpr size_t kMaxRequests=64,kMaxPerPlayer=4,kMaxResponses=128;
    explicit Worker(std::string path);
    ~Worker();
    Worker(const Worker&)=delete; Worker& operator=(const Worker&)=delete;
    bool Submit(Request request);
    bool Pop(Response& response);
    void Cancel(int playerId);
    void SetEnabled(bool enabled);
    void Stop(); // shutdown only, never from a packet/game callback
    int State() const { return state_.load(); } // 0 starting, 1 ready, -1 init failed
private:
    void Run();
    std::string path_;
    std::mutex mutex_; std::condition_variable wake_;
    std::deque<Request> requests_; std::deque<Response> responses_;
    std::unordered_map<int,Identity> identities_;
    std::atomic<int> state_{0}; bool stopping_=false,enabled_=true;
    std::thread thread_;
};
}
