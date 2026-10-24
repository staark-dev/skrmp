// M5.8 — main-thread phone transport/auth adapter; SQLite lives on the worker.
// Spec: docs/phone/PHONE-M4-PROTOCOL.md, docs/phone/PHONE-M4-DATABASE.md.
// Codec: shared/protocol/phone_protocol.h.

#include "skr_gate_component.hpp"
#include "protocol/phone_protocol.h"
#include "protocol/phone_profile.h"
#include "phone_m5_sql.h"
#include "protocol/skrx_phone.h"

#include <cstring>
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <string>
#include <unordered_set>

namespace skr::gate_omp {

using namespace protocol;
using namespace protocol::api;
using namespace protocol::phone;

static SkrGateComponent* g_phoneGate = nullptr;

// ── Helpers ─────────────────────────────────────────────────────────

static bool HasParams(const cell* params, int expected) {
    return params[0] >= static_cast<cell>(expected * sizeof(cell));
}

static constexpr ucell kUnpackedMax = (static_cast<ucell>(1) << ((sizeof(ucell) - 1) * 8)) - 1;

static bool ReadPawnString(AMX* amx, cell addr, char* out, size_t maxLen, size_t& len) {
    len = 0;
    if (!amx || addr < 0 || addr % static_cast<cell>(sizeof(cell)) != 0 || addr >= amx->stp) return false;
    unsigned char* data = amx->data ? amx->data
        : reinterpret_cast<unsigned char*>(amx->base) + reinterpret_cast<AMX_HEADER*>(amx->base)->dat;
    const cell* p = reinterpret_cast<const cell*>(data + addr);
    const size_t cellsAvail = static_cast<size_t>((amx->stp - addr) / static_cast<cell>(sizeof(cell)));
    if (!cellsAvail) return false;
    const bool packed = static_cast<ucell>(p[0]) > kUnpackedMax;
    if (packed) {
        for (size_t c = 0; c < cellsAvail; ++c) {
            const ucell v = static_cast<ucell>(p[c]);
            for (int b = static_cast<int>(sizeof(cell)) - 1; b >= 0; --b) {
                const char ch = static_cast<char>((v >> (b * 8)) & 0xFF);
                if (!ch) { out[len] = '\0'; return true; }
                if (len >= maxLen) return false;
                out[len++] = ch;
            }
        }
        return false;
    }
    for (size_t c = 0; c < cellsAvail; ++c) {
        if (p[c] == 0) { out[len] = '\0'; return true; }
        if (len >= maxLen) return false;
        out[len++] = (p[c] > 0 && p[c] < 256) ? static_cast<char>(p[c]) : '?';
    }
    return false;
}

static uint64_t PhoneNowMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()).count());
}

// ── Pawn natives ────────────────────────────────────────────────────

// native SKRE_PlayerAuthenticated(playerid, const username[]);
static cell AMX_NATIVE_CALL Native_PlayerAuthenticated(AMX* amx, const cell* params) {
    if (!g_phoneGate || !HasParams(params, 2)) return 0;
    char name[25];
    size_t nameLen = 0;
    if (!ReadPawnString(amx, params[2], name, sizeof(name) - 1, nameLen) || !nameLen) return 0;
    name[nameLen] = '\0';
    if (!g_phoneGate->PlayerAuthenticate(params[1], name)) return 0;
    // The client asked for PhoneSync at Welcome, usually before login finished
    // (answered NoPhone). Push its identity now that the nick is authoritative.
    g_phoneGate->PhonePushSync(params[1]);
    return 1;
}

// native SKRE_IsPlayerAuthenticated(playerid);
static cell AMX_NATIVE_CALL Native_IsPlayerAuthenticated(AMX*, const cell* params) {
    if (!g_phoneGate || !HasParams(params, 1)) return 0;
    return g_phoneGate->IsPlayerAuthenticated(params[1]) ? 1 : 0;
}

// native SKRE_PhoneEnable(bool:enable = true);
static cell AMX_NATIVE_CALL Native_PhoneEnable(AMX*, const cell* params) {
    if (!g_phoneGate || !HasParams(params, 1)) return 0;
    g_phoneGate->SetPhoneEnabled(params[1] != 0);
    return 1;
}

// Forward declarations for test natives (defined after SkrGateComponent methods)
static cell AMX_NATIVE_CALL Native_TestPhoneSync(AMX*, const cell*);
static cell AMX_NATIVE_CALL Native_TestSmsSend(AMX*, const cell*);

static const AMX_NATIVE_INFO kPhoneNatives[] = {
    {"SKRE_PlayerAuthenticated",   &Native_PlayerAuthenticated},
    {"SKRE_IsPlayerAuthenticated", &Native_IsPlayerAuthenticated},
    {"SKRE_PhoneEnable",           &Native_PhoneEnable},
    {"SKRE_TestPhoneSync",         &Native_TestPhoneSync},
    {"SKRE_TestSmsSend",           &Native_TestSmsSend},
};


// All SDK/network work stays on the main thread; SQLite only runs in Worker.
bool SkrGateComponent::PhoneDbInit() {
    if (phoneWorker_) { phoneWorker_->SetEnabled(true); return true; }
    try { phoneWorker_=std::make_unique<skr::phone_backend::Worker>("skr.db"); return true; }
    catch (...) { return false; }
}
void SkrGateComponent::PhoneDbClose() { phoneWorker_.reset(); }
void SkrGateComponent::PhoneOnPlayerDisconnect(int id) { if (phoneWorker_) phoneWorker_->Cancel(id); }

bool SkrGateComponent::PhoneSendCall(IPlayer& player,uint32_t sessionId,uint32_t callId,
                                    Op op,const uint8_t* data,size_t size) {
    uint32_t caps=0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it=players_.find(player.getID());
        if (it==players_.end() || !phoneEnabled_ || !it->second.authenticated || !it->second.api.ready ||
            it->second.api.quarantined || it->second.api.sessionId!=sessionId) return false;
        caps=it->second.api.capabilities;
    }
    namespace wire=protocol::skrx::phone;
    if (!(caps & CapPhone) || !(caps & CapabilityFor(op))) return false;
    if (wire::SelectTransport(caps)==wire::Transport::GatewayV1) {
        uint8_t frame[protocol::skrx::kMaxGatewaySize];
        const auto n=wire::Encode(static_cast<uint8_t>(op),sessionId,0,data,size,false,(caps&CapPhoneSetup)!=0,frame,sizeof(frame));
        return n && ApiSend(player,frame,n);
    }
    return ApiSendCall(player,sessionId,callId,op,data,size);
}

bool SkrGateComponent::PhoneQueueRequest(IPlayer& player,const ClientRequest& request,bool gateway,bool serverPush) {
    namespace wire=protocol::skrx::phone;
    skr::phone_backend::Request work;
    uint32_t callId=0;
    bool authenticated=false,allowed=false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it=players_.find(player.getID());
        if (it==players_.end()) return false;
        const auto& d=it->second; auto& a=it->second.api;
        if (!a.ready || a.quarantined || a.sessionId!=request.sessionId || !phoneEnabled_ || !(a.capabilities&CapPhone)) return false;
        if ((wire::SelectTransport(a.capabilities)==wire::Transport::GatewayV1)!=gateway) return false;
        if (!a.callLimiter.Allow(PhoneNowMs(),kMaxCallsPerSecond)) return false;
        work.identity.playerId=player.getID(); work.identity.generation=d.generation;
        work.identity.sessionId=a.sessionId; work.identity.gateway=gateway;
        work.identity.setup=(a.capabilities&CapPhoneSetup)!=0;
        std::memcpy(work.identity.owner,d.authName,sizeof(work.identity.owner));
        authenticated=d.authenticated;
        callId=a.nextCallId++;
        allowed=authenticated && phoneWorker_ &&
            wire::ValidateRequest(request.op,request.payload,request.payloadLen,work.identity.setup);
    }
    auto error=[&](ErrorCode code) {
        PhoneErrorResponse response; response.requestId=request.requestId; response.code=code;
        uint8_t data[100]; const auto size=EncodePhoneError(response,data,sizeof(data));
        // PhoneSendCall checks current auth. Pre-auth NoPhone is handled below.
        if (authenticated) PhoneSendCall(player,request.sessionId,callId,Op::PhoneError,data,size);
        else if (!gateway) ApiSendCall(player,request.sessionId,callId,Op::PhoneError,data,size);
    };
    if (!authenticated) {
        // Gateway pre-auth requests are ignored; login's queued sync recovers.
        if (!gateway) error(ErrorCode::NoPhone); return false;
    }
    if (!allowed || (!serverPush && !request.requestId)) { error(ErrorCode::InternalError); return false; }
    work.requestId=request.requestId; work.op=request.op; work.size=request.payloadLen;
    if (work.size) std::memcpy(work.data,request.payload,work.size);
    if (!phoneWorker_->Submit(std::move(work))) { error(ErrorCode::RateLimited); return false; }
    return true;
}
void SkrGateComponent::ApiOnClientRequest(IPlayer& player,const uint8_t* data,size_t len) {
    ClientRequest request;
    if (!DecodeClientRequest(data,len,request) || request.domain!=RequestDomain::Phone) {
        ApiInvalid(player.getID(),"malformed phone request"); return;
    }
    PhoneQueueRequest(player,request,false);
}
void SkrGateComponent::OnGatewayPhoneMessage(IPlayer& player,const protocol::skrx::GatewayHeader& hdr,
                                             const uint8_t* data,size_t len) {
    namespace wire=protocol::skrx::phone;
    const uint32_t caps=ApiCapabilities(player.getID());
    if (wire::SelectTransport(caps)!=wire::Transport::GatewayV1) return;
    wire::Frame frame;
    if (!wire::Decode(hdr,data,len,true,(caps&CapPhoneSetup)!=0,frame)) return;
    ClientRequest request; request.sessionId=frame.sessionId; request.requestId=frame.requestId;
    request.domain=RequestDomain::Phone; request.op=frame.op;
    request.payload=frame.body; request.payloadLen=static_cast<uint8_t>(frame.bodyLen);
    PhoneQueueRequest(player,request,true);
}
bool SkrGateComponent::PhonePushSync(int id) {
    if (!core_ || !phoneWorker_) return false;
    ClientRequest request; bool gateway=false;
    {
        std::lock_guard<std::mutex> lock(mutex_); auto it=players_.find(id);
        if (it==players_.end() || !it->second.authenticated || !it->second.api.ready) return false;
        request.sessionId=it->second.api.sessionId;
        gateway=protocol::skrx::phone::SelectTransport(it->second.api.capabilities)==protocol::skrx::phone::Transport::GatewayV1;
    }
    request.domain=RequestDomain::Phone; request.op=static_cast<uint8_t>(ClientOp::PhoneSync); request.requestId=0;
    if (auto* player=core_->getPlayers().get(id)) return PhoneQueueRequest(*player,request,gateway,true);
    return false;
}
void SkrGateComponent::PhoneTick() {
    if (!phoneWorker_ || !core_) return;
    skr::phone_backend::Response response;
    // Bounded main-thread delivery. No database/file I/O and no waiting here.
    for (int i=0;i<16 && phoneWorker_->Pop(response);++i) {
        int id=-1; uint32_t session=0,call=0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!phoneEnabled_) continue;
            auto select=[&](int pid,SkrPlayerData& d) {
                auto& a=d.api;
                if (!d.authenticated || !a.ready || a.quarantined || !(a.capabilities&CapPhone)) return false;
                if (response.ownerPush) {
                    if (std::strcmp(d.authName,response.recipient)) return false;
                } else {
                    const auto& target=response.identity;
                    if (pid!=target.playerId || d.generation!=target.generation || a.sessionId!=target.sessionId ||
                        std::strcmp(d.authName,target.owner) || !target.lease || !target.lease->load()) return false;
                }
                id=pid; session=a.sessionId; call=a.nextCallId++; return true;
            };
            if (!response.ownerPush) {
                auto it=players_.find(response.identity.playerId);
                if (it!=players_.end()) select(it->first,it->second);
            } else for (auto& pair:players_) if (select(pair.first,pair.second)) break;
        }
        if (id>=0) if (auto* player=core_->getPlayers().get(id))
            PhoneSendCall(*player,session,call,response.op,response.data,response.size);
    }
}
// Old development-only synchronous helpers cannot safely expose SQLite from a
// Pawn/network callback. Sync now queues; synchronous SMS command is disabled.
bool SkrGateComponent::TestPhoneSync(int id) { return PhonePushSync(id); }
int SkrGateComponent::TestSmsSend(int,int,const char*,size_t) { return 0; }

// ── Test Pawn natives ──────────────────────────────────────────────

// native SKRE_TestPhoneSync(playerid);
static cell AMX_NATIVE_CALL Native_TestPhoneSync(AMX*, const cell* params) {
    if (!g_phoneGate || !HasParams(params, 1)) return 0;
    return g_phoneGate->TestPhoneSync(params[1]) ? 1 : 0;
}

// native SKRE_TestSmsSend(playerid, targetid, const text[]);
static cell AMX_NATIVE_CALL Native_TestSmsSend(AMX* amx, const cell* params) {
    if (!g_phoneGate || !HasParams(params, 3)) return 0;
    char text[kMaxSmsText + 1];
    size_t textLen = 0;
    if (!ReadPawnString(amx, params[3], text, kMaxSmsText, textLen) || !textLen) return 0;
    text[textLen] = '\0';
    return g_phoneGate->TestSmsSend(params[1], params[2], text, textLen);
}

// ── Registration ────────────────────────────────────────────────────

void RegisterPhoneNatives(IPawnScript& script, SkrGateComponent* gate) {
    g_phoneGate = gate;
    script.Register(kPhoneNatives, static_cast<int>(sizeof(kPhoneNatives) / sizeof(kPhoneNatives[0])));
}

void ReleasePhoneNatives(SkrGateComponent* gate) {
    if (g_phoneGate == gate) g_phoneGate = nullptr;
}

} // namespace skr::gate_omp
