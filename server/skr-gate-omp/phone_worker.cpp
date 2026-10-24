#include "phone_worker.h"
#include <sqlite3.h>
#include <cstdlib>
#include <cstring>
#include <algorithm>

namespace skr::phone_backend {
long Result::getFieldInt(size_t i) const {
    if (row>=rows.size() || i>=rows[row].size()) return 0;
    return std::strtol(rows[row][i].c_str(),nullptr,10);
}
bool Database::Open(const std::string& path) {
    ownerThread_=std::this_thread::get_id();
    if (sqlite3_open_v2(path.c_str(),&db_,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|SQLITE_OPEN_NOMUTEX,nullptr)!=SQLITE_OK) { Close(); return false; }
    sqlite3_busy_timeout(db_,100); return true;
}
void Database::Close() { if (db_) sqlite3_close(db_); db_=nullptr; }
void Database::FinishRequest() noexcept {
    if (db_ && ownerThread_==std::this_thread::get_id() && !sqlite3_get_autocommit(db_))
        sqlite3_exec(db_,"ROLLBACK",nullptr,nullptr,nullptr);
    lease_.reset();
}
Result* Database::executeQuery(std::string_view sql) {
    if (!db_ || ownerThread_!=std::this_thread::get_id() || sql.size()>8192) return nullptr;
    if (lease_ && !lease_->load()) {
        sqlite3_exec(db_,"ROLLBACK",nullptr,nullptr,nullptr); return nullptr;
    }
    auto result=std::make_unique<Result>();
    const std::string query(sql);
    const auto callback=[](void* ptr,int count,char** values,char**) -> int {
        try {
        auto& rs=*static_cast<Result*>(ptr);
        if (rs.rows.size()>=128 || count>16) return 1;
        std::vector<std::string> row;
        for (int i=0;i<count;++i) {
            const size_t len=values[i]?std::strlen(values[i]):0;
            if (len>4096 || rs.bytes+len>65536) return 1;
            rs.bytes+=len; row.emplace_back(values[i]?values[i]:"");
        }
        rs.rows.push_back(std::move(row)); return 0;
        } catch (...) { return 1; }
    };
    if (sqlite3_exec(db_,query.c_str(),callback,result.get(),nullptr)!=SQLITE_OK) return nullptr;
    return result.release();
}
Worker::Worker(std::string path):path_(std::move(path)),thread_([this]{ Run(); }) {}
Worker::~Worker() { Stop(); }
void Worker::Stop() {
    { std::lock_guard<std::mutex> lock(mutex_); stopping_=true;
      for (auto& pair:identities_) pair.second.lease->store(false); requests_.clear(); }
    wake_.notify_all(); if (thread_.joinable()) thread_.join();
}
void Worker::Cancel(int id) {
    std::lock_guard<std::mutex> lock(mutex_); auto it=identities_.find(id);
    if (it!=identities_.end()) { it->second.lease->store(false); identities_.erase(it); }
}
void Worker::SetEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_); enabled_=enabled;
    if (!enabled) { for (auto& pair:identities_) pair.second.lease->store(false); identities_.clear(); requests_.clear(); responses_.clear(); }
    wake_.notify_all();
}
bool Worker::Submit(Request request) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || !enabled_ || request.identity.playerId<0 || !request.identity.sessionId ||
        !request.identity.owner[0] || request.identity.owner[24] || request.size>sizeof(request.data) ||
        (!request.requestId && request.op!=static_cast<uint8_t>(protocol::phone::ClientOp::PhoneSync)) ||
        requests_.size()>=kMaxRequests) return false;
    size_t pending=0;
    for (const auto& work:requests_) if (work.identity.playerId==request.identity.playerId) ++pending;
    if (pending>=kMaxPerPlayer) return false;
    auto& identity=identities_[request.identity.playerId];
    if (!identity.lease || identity.generation!=request.identity.generation || identity.sessionId!=request.identity.sessionId || std::strcmp(identity.owner,request.identity.owner)) {
        if (identity.lease) identity.lease->store(false);
        identity=request.identity; identity.lease=std::make_shared<std::atomic<bool>>(true);
    }
    request.identity.lease=identity.lease;
    requests_.push_back(std::move(request)); wake_.notify_one(); return true;
}
bool Worker::Pop(Response& result) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (responses_.empty()) return false;
    result=std::move(responses_.front()); responses_.pop_front(); wake_.notify_one(); return true;
}
void Worker::Run() {
    Store store; bool ready=false;
    try { ready=store.Initialize(path_); } catch (...) { ready=false; }
    state_.store(ready?1:-1);
    for (;;) {
        Request request;
        { std::unique_lock<std::mutex> lock(mutex_);
          wake_.wait(lock,[this]{ return stopping_ || !requests_.empty(); });
          if (stopping_) break;
          request=std::move(requests_.front()); requests_.pop_front(); }
        if (!request.identity.lease->load()) continue;
        std::vector<Response> output;
        try {
            if (ready) output=store.Process(request);
            else throw 0;
        } catch (...) {
            protocol::phone::PhoneErrorResponse error; error.requestId=request.requestId; error.code=protocol::phone::ErrorCode::InternalError;
            Response response; response.identity=request.identity;
            response.size=protocol::phone::EncodePhoneError(error,response.data,sizeof(response.data)); output.push_back(response);
        }
        { std::unique_lock<std::mutex> lock(mutex_);
          wake_.wait(lock,[&]{ return stopping_ || !enabled_ || responses_.size()+output.size()<=kMaxResponses; });
          if (stopping_) break;
          if (!enabled_) continue;
          for (auto& event:output) responses_.push_back(std::move(event)); }
    }
    store.Close();
}
}
