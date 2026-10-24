// SKR Native Runtime API — server side (TASK API-001).
// Spec: docs/protocol/SKR_NATIVE_API.md. Codec: shared/protocol/native_api.h.
//
// Threading: per-RPC handlers, onTick and Pawn natives all run on the open.mp
// main thread. Script callbacks are still never invoked from inside packet
// handling: they are queued in apiEvents_ and dispatched from onTick
// (ApiDispatchEvents), so a callback can safely call any SKRE_* native.
//
// Trust: everything from the client is untrusted. Replies are matched against
// server-held state (session id, pending call id, op) and only ever resolve
// what the server itself asked. No client message can change server policy.

#include "skr_gate_component.hpp"

#include <algorithm>
#include "protocol/skrx_phone.h"
#include <chrono>
#include <random>

namespace skr::gate_omp {

namespace api = protocol::api;

namespace {

uint64_t ApiNowMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

constexpr const char* kCbReady        = "OnSKREClientReady";
constexpr const char* kCbDisconnected = "OnSKREClientDisconnected";
constexpr const char* kCbConfirm      = "OnSKREConfirmResponse";

} // namespace

// ── Inbound (RPC 240, envelope steps 5..8) ───────────────────────────

void SkrGateComponent::OnApiMessage(IPlayer& player, NetworkBitStream& bs) {
    const int id = player.getID();
    const int bits = static_cast<int>(bs.GetNumberOfBitsUsed()) - static_cast<int>(bs.GetReadOffset());
    if (bits <= 0 || bits % 8 != 0 || bits / 8 > static_cast<int>(api::kMaxInboundSize)) {
        ApiInvalid(id, "bad length");
        return;
    }
    uint8_t buf[api::kMaxInboundSize];
    const size_t len = static_cast<size_t>(bits / 8);
    if (!bs.Read(reinterpret_cast<char*>(buf), static_cast<unsigned int>(len))) {
        ApiInvalid(id, "read failed");
        return;
    }
    if (!IsEnhanced(id)) return;   // API only exists on confirmed Enhanced sessions
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = players_.find(id);
        if (it == players_.end() || it->second.api.quarantined) return;
    }
    switch (static_cast<api::Step>(buf[1])) {
        case api::Step::Hello:         ApiOnHello(player, buf, len); break;
        case api::Step::Reply:         ApiOnReply(player, buf, len); break;
        case api::Step::ClientRequest: ApiOnClientRequest(player, buf, len); break;
        default:                       ApiInvalid(id, "server-to-client step from client"); break;
    }
}

void SkrGateComponent::ApiOnHello(IPlayer& player, const uint8_t* data, size_t len) {
    const int id = player.getID();
    api::Hello hello;
    if (!api::DecodeHello(data, len, hello)) { ApiInvalid(id, "malformed hello"); return; }

    api::Welcome welcome;
    bool sendHud = false, tokenMismatch = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = players_.find(id);
        if (it == players_.end()) return;
        SkrPlayerData& d = it->second;
        welcome.version = api::ChooseVersion(hello.versionMin, hello.versionMax);
        if (hello.enhancedToken != d.enhanced.token) {
            tokenMismatch = true;   // not bound to this connection's Enhanced handshake
        } else if (d.api.ready) {
            core_->printLn("[SKR API] player=%d event=hello-ignored reason=duplicate session=%08x",
                           id, d.api.sessionId);
            return;
        } else if (welcome.version == 0) {
            welcome.status = api::WelcomeStatus::VersionUnsupported;
        } else {
            d.api.ready = true;
            d.api.version = welcome.version;
            d.api.sessionId = ApiNewSessionId();
            d.api.capabilities = hello.capabilities & api::kKnownCapabilities;
            d.api.capabilities = protocol::skrx::phone::NormalizeCapabilities(d.api.capabilities,phoneEnabled_,SKR_PHONE_GATEWAY_ENABLED!=0);
            d.api.nextCallId = 1;
            welcome.sessionId = d.api.sessionId;
            welcome.capabilities = d.api.capabilities;
            apiEvents_.push_back({ApiEvent::Kind::Ready, id, d.api.sessionId, 0, 0});
            sendHud = d.api.hudVitalsNumbers >= 0 && (d.api.capabilities & api::CapHudValue);
        }
    }
    if (tokenMismatch) { ApiInvalid(id, "hello token mismatch"); return; }
    uint8_t out[api::kWelcomeSize];
    const size_t n = api::EncodeWelcome(welcome, out, sizeof(out));
    const bool sent = n && ApiSend(player, out, n);
    if (welcome.status != api::WelcomeStatus::Accepted) {
        core_->printLn("[SKR API] player=%d event=hello-refused client_versions=%u-%u server_versions=%u-%u",
                       id, hello.versionMin, hello.versionMax, api::kVersionMin, api::kVersionMax);
        return;
    }
    core_->printLn("[SKR API] player=%d event=ready version=%u session=%08x caps=0x%x client_caps=0x%x welcome=%s",
                   id, welcome.version, welcome.sessionId, welcome.capabilities, hello.capabilities,
                   sent ? "sent" : "NOT sent");
    if (sendHud) ApiSendHudOverride(id);

    if (welcome.capabilities & api::CapGateway) {
        SendVehicleRegistryToPlayer(player);
        SendVehicleBatchAssignToPlayer(player);
    }
}

void SkrGateComponent::ApiOnReply(IPlayer& player, const uint8_t* data, size_t len) {
    const int id = player.getID();
    api::Reply reply;
    if (!api::DecodeReply(data, len, reply)) { ApiInvalid(id, "malformed reply"); return; }

    api::ReplyCheck check;
    int32_t requestId = 0;
    uint32_t sessionId = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = players_.find(id);
        if (it == players_.end()) return;
        SkrApiState& a = it->second.api;
        if (!a.ready) { check = api::ReplyCheck::StaleSession; }
        else if (!a.replyLimiter.Allow(ApiNowMs(), api::kMaxRepliesPerSecond)) {
            core_->printLn("[SKR API] player=%d event=reply-dropped reason=rate-limit", id);
            return;
        } else {
            check = api::CheckConfirmReply(a.confirm, a.sessionId, reply);
            if (check == api::ReplyCheck::Accepted) {
                requestId = a.confirm.requestId;
                sessionId = a.sessionId;
                a.confirm = {};   // resolved exactly once: later copies are "unknown-call"
                apiEvents_.push_back({ApiEvent::Kind::ConfirmResponse, id, sessionId, requestId,
                                      static_cast<int32_t>(api::ToScriptResponse(reply.result))});
            }
        }
    }
    if (check != api::ReplyCheck::Accepted) {
        ApiInvalid(id, api::ReplyCheckName(check));
        return;
    }
    core_->printLn("[SKR API] player=%d event=confirm-reply call=%u request=%d result=%u",
                   id, reply.callId, requestId, static_cast<unsigned>(reply.result));
}

void SkrGateComponent::ApiInvalid(int playerId, const char* reason) {
    bool log = false, quarantine = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = players_.find(playerId);
        if (it == players_.end()) return;
        SkrApiState& a = it->second.api;
        if (a.quarantined) return;
        ++a.invalidMessages;
        log = a.invalidMessages <= 5;   // bounded log volume per session
        if (a.invalidMessages >= api::kMaxInvalidMessages) { a.quarantined = true; quarantine = true; }
    }
    if (log) core_->printLn("[SKR API] player=%d event=invalid reason=%s", playerId, reason);
    if (quarantine)
        core_->printLn("[SKR API] player=%d event=quarantined invalid=%u (API traffic ignored for this connection)",
                       playerId, api::kMaxInvalidMessages);
}

// ── Outbound (RPC 241) ───────────────────────────────────────────────

bool SkrGateComponent::ApiSend(IPlayer& player, const uint8_t* data, size_t len) {
    NetworkBitStream bs;
    bs.Write(reinterpret_cast<const char*>(data), static_cast<unsigned int>(len));
    return player.sendRPC(protocol::kRpcSessionInfo,
        Span<uint8_t>(bs.GetData(), bs.GetNumberOfBitsUsed()), OrderingChannel_SyncRPC);
}

bool SkrGateComponent::ApiSendCall(IPlayer& player, uint32_t sessionId, uint32_t callId,
                                   api::Op op, const uint8_t* payload, size_t payloadLen) {
    uint8_t out[api::kMaxCallSize];
    const size_t n = api::EncodeCall(sessionId, callId, op, payload, payloadLen, out, sizeof(out));
    return n && ApiSend(player, out, n);
}

uint32_t SkrGateComponent::ApiNewSessionId() {
    // Not a secret (the RakNet connection already identifies the peer); it only has to
    // differ between sessions so that anything from an earlier session is rejected.
    static std::mt19937 rng{std::random_device{}()};
    uint32_t id = 0;
    while (id == 0) id = rng() ^ (++apiSessionCounter_ * 0x9E3779B9u);
    return id;
}

bool SkrGateComponent::ApiIsReady(int playerId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = players_.find(playerId);
    return it != players_.end() && it->second.state != SkrPlayerState::Rejected && it->second.api.ready &&
           !it->second.api.quarantined;
}

uint32_t SkrGateComponent::ApiCapabilities(int playerId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = players_.find(playerId);
    if (it == players_.end() || !it->second.api.ready || it->second.api.quarantined) return 0;
    return it->second.api.capabilities;
}

bool SkrGateComponent::ApiNotify(int playerId, api::NotifyKind kind, uint8_t durationSec,
                                 const char* text, size_t len) {
    api::Notify m;
    m.kind = kind;
    m.durationSec = durationSec;
    if (!len || len > api::kMaxNotifyText) return false;
    memcpy(m.text, text, len);
    m.textLen = static_cast<uint8_t>(len);
    api::SanitizeText(m.text, m.textLen);
    uint8_t payload[api::kMaxCallPayload];
    const size_t pn = api::EncodeNotify(m, payload, sizeof(payload));
    if (!pn) return false;

    uint32_t sessionId = 0, callId = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = players_.find(playerId);
        if (it == players_.end() || it->second.state == SkrPlayerState::Rejected) return false;
        SkrApiState& a = it->second.api;
        if (!a.ready || a.quarantined || !(a.capabilities & api::CapNotify)) return false;
        if (!a.callLimiter.Allow(ApiNowMs(), api::kMaxCallsPerSecond)) return false;
        sessionId = a.sessionId;
        callId = a.nextCallId++;
    }
    IPlayer* player = core_->getPlayers().get(playerId);
    if (!player) return false;
    const bool sent = ApiSendCall(*player, sessionId, callId, api::Op::Notify, payload, pn);
    core_->printLn("[SKR API] player=%d event=notify call=%u kind=%u len=%u %s",
                   playerId, callId, static_cast<unsigned>(kind), static_cast<unsigned>(len),
                   sent ? "sent" : "NOT sent");
    return sent;
}

bool SkrGateComponent::ApiShowConfirm(int playerId, int32_t requestId, const char* title, size_t titleLen,
                                      const char* message, size_t messageLen, uint8_t timeoutSec) {
    api::Confirm m;
    if (!titleLen || titleLen > api::kMaxConfirmTitle || !messageLen || messageLen > api::kMaxConfirmMessage)
        return false;
    m.timeoutSec = timeoutSec;
    memcpy(m.title, title, titleLen);
    m.titleLen = static_cast<uint8_t>(titleLen);
    memcpy(m.message, message, messageLen);
    m.messageLen = static_cast<uint8_t>(messageLen);
    api::SanitizeText(m.title, m.titleLen);
    api::SanitizeText(m.message, m.messageLen);
    uint8_t payload[api::kMaxCallPayload];
    const size_t pn = api::EncodeConfirm(m, payload, sizeof(payload));
    if (!pn) return false;

    uint32_t sessionId = 0, callId = 0;
    const uint64_t now = ApiNowMs();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = players_.find(playerId);
        if (it == players_.end() || it->second.state == SkrPlayerState::Rejected) return false;
        SkrApiState& a = it->second.api;
        if (!a.ready || a.quarantined || !(a.capabilities & api::CapConfirm)) return false;
        if (a.confirm.active) return false;   // one outstanding confirm per player
        if (!a.callLimiter.Allow(now, api::kMaxCallsPerSecond)) return false;
        sessionId = a.sessionId;
        callId = a.nextCallId++;
        a.confirm.active = true;
        a.confirm.callId = callId;
        a.confirm.requestId = requestId;
        a.confirm.deadlineMs = now + static_cast<uint64_t>(timeoutSec) * 1000 + api::kConfirmGraceMs;
    }
    IPlayer* player = core_->getPlayers().get(playerId);
    const bool sent = player && ApiSendCall(*player, sessionId, callId, api::Op::Confirm, payload, pn);
    if (!sent) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = players_.find(playerId);
        if (it != players_.end() && it->second.api.confirm.callId == callId) it->second.api.confirm = {};
    }
    core_->printLn("[SKR API] player=%d event=confirm-request call=%u request=%d timeout=%us %s",
                   playerId, callId, requestId, timeoutSec, sent ? "sent" : "NOT sent");
    return sent;
}

bool SkrGateComponent::ApiCancelConfirm(int playerId) {
    uint32_t sessionId = 0, callId = 0, target = 0;
    int32_t requestId = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = players_.find(playerId);
        if (it == players_.end()) return false;
        SkrApiState& a = it->second.api;
        if (!a.ready || !a.confirm.active) return false;
        target = a.confirm.callId;
        requestId = a.confirm.requestId;
        a.confirm = {};   // a reply that is already in flight becomes "unknown-call"
        sessionId = a.sessionId;
        callId = a.nextCallId++;
    }
    uint8_t payload[4];
    const size_t pn = api::EncodeCancelConfirm(target, payload, sizeof(payload));
    IPlayer* player = core_->getPlayers().get(playerId);
    const bool sent = player && pn && ApiSendCall(*player, sessionId, callId, api::Op::CancelConfirm, payload, pn);
    core_->printLn("[SKR API] player=%d event=confirm-cancel target=%u request=%d %s",
                   playerId, target, requestId, sent ? "sent" : "NOT sent");
    return true;   // the server side is cancelled either way
}

bool SkrGateComponent::ApiSetHudVisible(int playerId, api::HudComponent component, bool visible) {
    if (component != api::HudComponent::VitalsNumbers) return false;
    bool ready = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = players_.find(playerId);
        if (it == players_.end() || it->second.state == SkrPlayerState::Rejected) return false;
        it->second.api.hudVitalsNumbers = visible ? 1 : 0;   // stored: re-sent when the API becomes ready
        ready = it->second.api.ready;
    }
    if (ready) ApiSendHudOverride(playerId);
    return true;
}

void SkrGateComponent::ApiSendHudOverride(int playerId) {
    uint32_t sessionId = 0, callId = 0;
    api::HudSet m;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = players_.find(playerId);
        if (it == players_.end()) return;
        SkrApiState& a = it->second.api;
        if (!a.ready || a.quarantined || !(a.capabilities & api::CapHudValue) || a.hudVitalsNumbers < 0) return;
        // HUD values are state, not events: not subject to the call rate limit (always latest value).
        m.value = static_cast<uint8_t>(a.hudVitalsNumbers);
        sessionId = a.sessionId;
        callId = a.nextCallId++;
    }
    uint8_t payload[2];
    const size_t pn = api::EncodeHudSet(m, payload, sizeof(payload));
    IPlayer* player = core_->getPlayers().get(playerId);
    const bool sent = player && pn && ApiSendCall(*player, sessionId, callId, api::Op::HudSet, payload, pn);
    core_->printLn("[SKR API] player=%d event=hud-set component=vitals_numbers value=%u %s",
                   playerId, m.value, sent ? "sent" : "NOT sent");
}

// ── Lifecycle ────────────────────────────────────────────────────────

void SkrGateComponent::ApiTick(uint64_t nowMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [id, d] : players_) {
        SkrApiState& a = d.api;
        if (!a.confirm.active || nowMs < a.confirm.deadlineMs) continue;
        apiEvents_.push_back({ApiEvent::Kind::ConfirmResponse, id, a.sessionId, a.confirm.requestId,
                              static_cast<int32_t>(api::ScriptResponse::Timeout)});
        core_->printLn("[SKR API] player=%d event=confirm-timeout call=%u request=%d (server deadline)",
                       id, a.confirm.callId, a.confirm.requestId);
        a.confirm = {};
    }
}

void SkrGateComponent::ApiDispatchEvents() {
    std::vector<ApiEvent> events;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (apiEvents_.empty()) return;
        events.swap(apiEvents_);
        // Drop events whose session no longer exists (disconnect/reconnect in between).
        auto stale = [&](const ApiEvent& e) {
            auto it = players_.find(e.playerId);
            return it == players_.end() || !it->second.api.ready || it->second.api.sessionId != e.sessionId;
        };
        events.erase(std::remove_if(events.begin(), events.end(), stale), events.end());
    }
    if (!pawn_) return;
    for (const ApiEvent& e : events) {
        auto call = [&](IPawnScript* script) {
            if (!script) return;
            if (e.kind == ApiEvent::Kind::Ready)
                script->Call(kCbReady, DefaultReturnValue_True, static_cast<cell>(e.playerId));
            else
                script->Call(kCbConfirm, DefaultReturnValue_True, static_cast<cell>(e.playerId),
                             static_cast<cell>(e.requestId), static_cast<cell>(e.response));
        };
        call(pawn_->mainScript());
        for (IPawnScript* fs : pawn_->sideScripts()) call(fs);
    }
}

// Runs on the main thread from onPlayerDisconnect, after the player's data is
// erased: any pending confirm is cancelled with it (no response callback).
void SkrGateComponent::ApiOnDisconnect(int playerId, bool wasReady, bool hadConfirm) {
    if (!wasReady) return;
    core_->printLn("[SKR API] player=%d event=disconnected pending_confirm=%s",
                   playerId, hadConfirm ? "cancelled" : "none");
    if (!pawn_) return;
    if (IPawnScript* gm = pawn_->mainScript())
        gm->Call(kCbDisconnected, DefaultReturnValue_True, static_cast<cell>(playerId));
    for (IPawnScript* fs : pawn_->sideScripts())
        if (fs) fs->Call(kCbDisconnected, DefaultReturnValue_True, static_cast<cell>(playerId));
}

// ── Pawn natives (server/gamemode-integration/skr_enhanced.inc) ──────

namespace {

SkrGateComponent* g_apiGate = nullptr;
constexpr ucell kUnpackedMax = (static_cast<ucell>(1) << ((sizeof(ucell) - 1) * 8)) - 1;

bool HasParams(const cell* params, int count) {
    return params[0] >= static_cast<cell>(count * sizeof(cell));
}

// Copies a Pawn string (packed or unpacked) into out. Bounds-checked against the
// script's data/heap/stack segment. Fails if longer than maxLen.
bool ReadString(AMX* amx, cell addr, char* out, size_t maxLen, size_t& len) {
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
        return false;   // unterminated
    }
    for (size_t c = 0; c < cellsAvail; ++c) {
        if (p[c] == 0) { out[len] = '\0'; return true; }
        if (len >= maxLen) return false;
        out[len++] = (p[c] > 0 && p[c] < 256) ? static_cast<char>(p[c]) : '?';
    }
    return false;
}

// native SKRE_IsClientReady(playerid);
cell AMX_NATIVE_CALL Native_IsClientReady(AMX*, const cell* params) {
    if (!g_apiGate || !HasParams(params, 1)) return 0;
    return g_apiGate->ApiIsReady(params[1]) ? 1 : 0;
}

// native SKRE_GetClientCapabilities(playerid);
cell AMX_NATIVE_CALL Native_GetClientCapabilities(AMX*, const cell* params) {
    if (!g_apiGate || !HasParams(params, 1)) return 0;
    return static_cast<cell>(g_apiGate->ApiCapabilities(params[1]));
}

// native SKRE_ShowNotification(playerid, const text[], SKRE_NOTIFY:kind = SKRE_NOTIFY_INFO, duration = 5);
cell AMX_NATIVE_CALL Native_ShowNotification(AMX* amx, const cell* params) {
    if (!g_apiGate || !HasParams(params, 4)) return 0;
    if (params[3] < 0 || params[3] > 3) return 0;
    if (params[4] < api::kNotifyMinSec || params[4] > api::kNotifyMaxSec) return 0;
    char text[api::kMaxNotifyText + 1];
    size_t len = 0;
    if (!ReadString(amx, params[2], text, api::kMaxNotifyText, len) || !len) return 0;
    return g_apiGate->ApiNotify(params[1], static_cast<api::NotifyKind>(params[3]),
                                static_cast<uint8_t>(params[4]), text, len) ? 1 : 0;
}

// native SKRE_ShowConfirm(playerid, requestid, const title[], const message[], timeout = 30);
cell AMX_NATIVE_CALL Native_ShowConfirm(AMX* amx, const cell* params) {
    if (!g_apiGate || !HasParams(params, 5)) return 0;
    if (params[5] < api::kConfirmMinSec || params[5] > api::kConfirmMaxSec) return 0;
    char title[api::kMaxConfirmTitle + 1], message[api::kMaxConfirmMessage + 1];
    size_t titleLen = 0, messageLen = 0;
    if (!ReadString(amx, params[3], title, api::kMaxConfirmTitle, titleLen) || !titleLen) return 0;
    if (!ReadString(amx, params[4], message, api::kMaxConfirmMessage, messageLen) || !messageLen) return 0;
    return g_apiGate->ApiShowConfirm(params[1], static_cast<int32_t>(params[2]), title, titleLen,
                                     message, messageLen, static_cast<uint8_t>(params[5])) ? 1 : 0;
}

// native SKRE_CancelConfirm(playerid);
cell AMX_NATIVE_CALL Native_CancelConfirm(AMX*, const cell* params) {
    if (!g_apiGate || !HasParams(params, 1)) return 0;
    return g_apiGate->ApiCancelConfirm(params[1]) ? 1 : 0;
}

// native SKRE_SetHudVisible(playerid, SKRE_HUD:component, bool:visible);
cell AMX_NATIVE_CALL Native_SetHudVisible(AMX*, const cell* params) {
    if (!g_apiGate || !HasParams(params, 3)) return 0;
    if (params[2] != static_cast<cell>(api::HudComponent::VitalsNumbers)) return 0;
    return g_apiGate->ApiSetHudVisible(params[1], api::HudComponent::VitalsNumbers, params[3] != 0) ? 1 : 0;
}

const AMX_NATIVE_INFO kApiNatives[] = {
    {"SKRE_IsClientReady", &Native_IsClientReady},
    {"SKRE_GetClientCapabilities", &Native_GetClientCapabilities},
    {"SKRE_ShowNotification", &Native_ShowNotification},
    {"SKRE_ShowConfirm", &Native_ShowConfirm},
    {"SKRE_CancelConfirm", &Native_CancelConfirm},
    {"SKRE_SetHudVisible", &Native_SetHudVisible},
};

} // namespace

void RegisterApiNatives(IPawnScript& script, SkrGateComponent* gate) {
    g_apiGate = gate;
    script.Register(kApiNatives, static_cast<int>(sizeof(kApiNatives) / sizeof(kApiNatives[0])));
}

void ReleaseApiNatives(SkrGateComponent* gate) {
    if (g_apiGate == gate) g_apiGate = nullptr;
}

} // namespace skr::gate_omp
