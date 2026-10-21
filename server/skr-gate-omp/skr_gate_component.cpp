#include "skr_gate_component.hpp"
#include "protocol/join_validation.h"
#include <Server/Components/Variables/variables.hpp>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

namespace skr::gate_omp {

// ── RPC 240: client state from SKRClient.dll ──────────────

namespace {

constexpr uint64_t kClientStateMinIntervalMs = 50;

uint64_t NowMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

bool InRange(float v, float lo, float hi) { return std::isfinite(v) && v >= lo && v <= hi; }

// Zone names end up inside JSON on the server: keep printable ASCII, drop quotes/backslashes.
bool ReadName(NetworkBitStream& bs, char (&out)[protocol::kClientStateMaxName + 1]) {
    uint8_t len = 0;
    if (!bs.readUINT8(len) || len > protocol::kClientStateMaxName) return false;
    if (len && !bs.Read(out, len)) return false;
    out[len] = '\0';
    for (uint8_t i = 0; i < len; ++i) {
        char c = out[i];
        if (c < 0x20 || c > 0x7E || c == '"' || c == '\\') out[i] = ' ';
    }
    return true;
}

} // namespace

bool SkrClientStateHandler::onReceive(IPlayer& peer, NetworkBitStream& bs) {
    const int id = peer.getID();
    if (!gate_) return false;
    const int offset = bs.GetReadOffset();
    uint8_t marker = 0;
    if (!bs.readUINT8(marker)) return false;
    bs.SetReadOffset(offset);
    if (marker == protocol::kEnhancedMarker) { gate_->Negotiate(peer, bs); return false; }
    if (!gate_->IsEnhanced(id)) return false;

    const uint64_t now = NowMs();
    auto it = lastAcceptMs_.find(id);
    if (it != lastAcceptMs_.end() && now - it->second < kClientStateMinIntervalMs) return false;

    uint8_t version = 0;
    float x, y, z, heading, range, orient;
    uint16_t speed = 0, maxSpeed = 0;
    int8_t gear = 0;
    char city[protocol::kClientStateMaxName + 1], area[protocol::kClientStateMaxName + 1];
    if (!bs.readUINT8(version) || version != protocol::kClientStateVersion) return false;
    if (!bs.readFLOAT(x) || !bs.readFLOAT(y) || !bs.readFLOAT(z) || !bs.readFLOAT(heading)
        || !bs.readFLOAT(range) || !bs.readFLOAT(orient)) return false;
    if (!bs.readUINT16(speed) || !bs.readINT8(gear) || !bs.readUINT16(maxSpeed)) return false;
    if (!ReadName(bs, city) || !ReadName(bs, area)) return false;

    if (!InRange(x, -20000.f, 20000.f) || !InRange(y, -20000.f, 20000.f) || !InRange(z, -5000.f, 5000.f)
        || !InRange(heading, -1.f, 361.f) || !InRange(range, 1.f, 10000.f) || !InRange(orient, -181.f, 181.f)
        || speed > 2000 || gear < -1 || gear > 10 || maxSpeed > 2000) {
        return false;
    }

    auto* vars = queryExtension<IPlayerVariableData>(peer);
    if (!vars) return false;
    lastAcceptMs_[id] = now;
    vars->setFloat("skr_x", x);
    vars->setFloat("skr_y", y);
    vars->setFloat("skr_z", z);
    vars->setFloat("skr_heading", heading);
    vars->setFloat("skr_range", range);
    vars->setFloat("skr_orient", orient);
    vars->setInt("skr_speed", speed);
    vars->setInt("skr_gear", gear);
    vars->setInt("skr_maxspeed", maxSpeed);
    vars->setString("skr_city", city);
    vars->setString("skr_area", area);
    vars->setInt("skr_state_seq", ++seq_[id]);   // set last: scripts read the rest when it changes
    return false;   // consumed: nothing else handles SKR-private RPC 240
}

// ── RPC 25 filter: block non-SKR clients at join ────────

// Standard layout: open.mp 3d20f3dc Shared/NetCode/core.hpp PlayerConnect.
bool SkrJoinFilter::onReceive(IPlayer& peer, NetworkBitStream& bs) {
    if (!protocol::ValidateStandardJoin(bs)) return false;
    return gate_ && gate_->AdmitInitial(peer);
}

bool SkrRadarRectHandler::onReceive(IPlayer& peer, NetworkBitStream& bs) {
    const int id = peer.getID();
    if (!gate_ || !gate_->IsEnhanced(id)) return false;

    const uint64_t now = NowMs();
    auto it = lastAcceptMs_.find(id);
    if (it != lastAcceptMs_.end() && now - it->second < kClientStateMinIntervalMs) return false;

    uint8_t version = 0;
    float x, y, w, h;
    if (!bs.readUINT8(version) || version != protocol::kRadarRectVersion) return false;
    if (!bs.readFLOAT(x) || !bs.readFLOAT(y) || !bs.readFLOAT(w) || !bs.readFLOAT(h)) return false;

    if (!InRange(x, 0.f, 8192.f) || !InRange(y, 0.f, 8192.f) ||
        !InRange(w, 1.f, 8192.f) || !InRange(h, 1.f, 8192.f)) return false;

    lastAcceptMs_[id] = now;
    gate_->OnRadarRect(id, x, y, w, h);
    return false;
}

void SkrGateComponent::OnRadarRect(int playerId, float x, float y, float w, float h) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = players_.find(playerId);
        if (it == players_.end()) return;
        it->second.radarRectX = x;
        it->second.radarRectY = y;
        it->second.radarRectW = w;
        it->second.radarRectH = h;
        it->second.radarRectReceived = true;
    }

    auto* vars = queryExtension<IPlayerVariableData>(*core_->getPlayers().get(playerId));
    if (vars) {
        vars->setFloat("skr_radar_x", x);
        vars->setFloat("skr_radar_y", y);
        vars->setFloat("skr_radar_w", w);
        vars->setFloat("skr_radar_h", h);
    }

    if (pawn_) {
        cell cx, cy, cw, ch;
        std::memcpy(&cx, &x, sizeof(cell));
        std::memcpy(&cy, &y, sizeof(cell));
        std::memcpy(&cw, &w, sizeof(cell));
        std::memcpy(&ch, &h, sizeof(cell));
        if (IPawnScript* gm = pawn_->mainScript())
            gm->Call("OnPlayerRadarRect", DefaultReturnValue_True, playerId, cx, cy, cw, ch);
        for (IPawnScript* fs : pawn_->sideScripts())
            if (fs) fs->Call("OnPlayerRadarRect", DefaultReturnValue_True, playerId, cx, cy, cw, ch);
    }
}

bool SkrGateComponent::GetRadarRect(int playerId, float& x, float& y, float& w, float& h) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = players_.find(playerId);
    if (it == players_.end() || !it->second.radarRectReceived) return false;
    x = it->second.radarRectX;
    y = it->second.radarRectY;
    w = it->second.radarRectW;
    h = it->second.radarRectH;
    return true;
}

// ── Pawn natives: SKR_SetRadarStyle / SKR_SetRadarSize (skr_gate.inc) ──

namespace {

SkrGateComponent* g_gate = nullptr;

bool HasArgs(const cell* params, int count) {
    return params[0] >= static_cast<cell>(count * sizeof(cell));
}

float CellToFloat(cell c) {
    float f;
    std::memcpy(&f, &c, sizeof(f));
    return f;
}

// native SKR_SetRadarStyle(playerid, SKR_RADAR_STYLE:style);
cell AMX_NATIVE_CALL Native_SetRadarStyle(AMX*, const cell* params) {
    if (!g_gate || !HasArgs(params, 2) || params[2] < 0 || params[2] > 1) return 0;
    return g_gate->SetRadarStyle(params[1], static_cast<protocol::RadarStyle>(params[2])) ? 1 : 0;
}

// native SKR_SetRadarSize(playerid, SKR_RADAR_SIZE:mode, Float:scale = 1.0);
cell AMX_NATIVE_CALL Native_SetRadarSize(AMX*, const cell* params) {
    if (!g_gate || !HasArgs(params, 3) || params[2] < 0 || params[2] > 1) return 0;
    return g_gate->SetRadarSize(params[1], static_cast<protocol::RadarSizeMode>(params[2]),
                                CellToFloat(params[3])) ? 1 : 0;
}

// native SKR_GetRadarRect(playerid, &Float:x, &Float:y, &Float:w, &Float:h);
cell AMX_NATIVE_CALL Native_GetRadarRect(AMX* amx, const cell* params) {
    if (!g_gate || !HasArgs(params, 5)) return 0;
    float x, y, w, h;
    if (!g_gate->GetRadarRect(params[1], x, y, w, h)) return 0;
    // Resolve AMX reference parameters via amx->data/amx->base.
    unsigned char* data = (amx->data != nullptr) ? amx->data :
        (reinterpret_cast<unsigned char*>(amx->base) + reinterpret_cast<AMX_HEADER*>(amx->base)->dat);
    auto ref = [&](cell addr) -> cell* { return reinterpret_cast<cell*>(data + addr); };
    std::memcpy(ref(params[2]), &x, sizeof(cell));
    std::memcpy(ref(params[3]), &y, sizeof(cell));
    std::memcpy(ref(params[4]), &w, sizeof(cell));
    std::memcpy(ref(params[5]), &h, sizeof(cell));
    return 1;
}

const AMX_NATIVE_INFO kNatives[] = {
    {"SKR_SetRadarStyle", &Native_SetRadarStyle},
    {"SKR_SetRadarSize", &Native_SetRadarSize},
    {"SKR_GetRadarRect", &Native_GetRadarRect},
};

} // namespace

void SkrGateComponent::onAmxLoad(IPawnScript& script) {
    script.Register(kNatives, static_cast<int>(sizeof(kNatives) / sizeof(kNatives[0])));
}

SkrGateComponent::~SkrGateComponent() {
    if (g_gate == this) g_gate = nullptr;
    if (pawn_) pawn_->getEventDispatcher().removeEventHandler(this);
    if (core_) {
        core_->removePerRPCInEventHandler<kRPC_ClientJoin>(&joinFilter_);
        core_->removePerRPCInEventHandler<protocol::kRpcClientState>(&clientStateHandler_);
        core_->removePerRPCInEventHandler<protocol::kRpcRadarRect>(&radarRectHandler_);
        core_->getEventDispatcher().removeEventHandler(this);
        core_->getPlayers().getPlayerConnectDispatcher().removeEventHandler(this);
    }
}

// ── IComponent lifecycle ─────────────────────────────────

void SkrGateComponent::onLoad(ICore* c) {
    core_ = c;
    g_gate = this;
    core_->printLn("SKR Gate component loaded (v0.1.0)");

    if (!clientPolicy_.LoadFromFile("skr-runtime.json", clientPolicyError_)) {
        core_->logLn(LogLevel::Error, "[SKR ClientPolicy] initialization failed: %s",
                     clientPolicyError_.c_str());
        core_->logLn(LogLevel::Error, "[SKR ClientPolicy] fail-closed: all new clients will be rejected");
    }
    for (const std::string& warning : clientPolicy_.Warnings()) {
        core_->logLn(LogLevel::Warning, "[SKR ClientPolicy] %s", warning.c_str());
    }
}

void SkrGateComponent::onInit(IComponentList* components) {
    pawn_ = components->queryComponent<IPawnComponent>();
    if (pawn_) {
        pawn_->getEventDispatcher().addEventHandler(this);
        // Scripts loaded before this handler was added get the natives now.
        if (IPawnScript* gm = pawn_->mainScript()) onAmxLoad(*gm);
        for (IPawnScript* fs : pawn_->sideScripts()) if (fs) onAmxLoad(*fs);
    }
    core_->getPlayers().getPlayerConnectDispatcher().addEventHandler(this);
    core_->getEventDispatcher().addEventHandler(this);

    // Register per-RPC handler for CLIENT_JOIN (RPC 25) — high priority to run first
    joinFilter_.setOwner(this);
    core_->addPerRPCInEventHandler<kRPC_ClientJoin>(&joinFilter_, EventPriority_FairlyHigh);

    clientStateHandler_.setOwner(this, core_);
    core_->addPerRPCInEventHandler<protocol::kRpcClientState>(&clientStateHandler_);

    radarRectHandler_.setOwner(this, core_);
    core_->addPerRPCInEventHandler<protocol::kRpcRadarRect>(&radarRectHandler_);

    core_->printLn("SKR Gate: registered player event handlers + RPC filter + client-state RPC %d + radar-rect RPC %d",
                   protocol::kRpcClientState, protocol::kRpcRadarRect);
}

void SkrGateComponent::onReady() {
    core_->printLn("SKR Gate: ready, enforcing ticket-based auth");

    // Advertise only the effective, enforced policy. LegacyNetwork keeps the
    // branded version rule and allows this trusted component to update allowed_clients.
    bool skrPublished = false;
    bool clientsPublished = false;
    bool modePublished = false;
    bool artworkPublished = false;
    const std::string advertised = clientPolicy_.AdvertisedClients();
    const auto& artwork = clientPolicy_.Artwork();
    for (INetwork* network : core_->getNetworks()) {
        if (auto* query = queryExtension<INetworkQueryExtension>(network)) {
            skrPublished = query->addRule(protocol::kSkrQueryRule, protocol::kSkrQueryRuleValue) || skrPublished;
            clientsPublished = query->addRule("allowed_clients", advertised) || clientsPublished;
            modePublished = query->addRule("skr_mode", clientPolicy_.ModeName()) || modePublished;
            if (artwork.HasArtwork()) {
                if (!artwork.bannerUrl.empty())
                    query->addRule("banner", artwork.bannerUrl);
                if (!artwork.logoUrl.empty())
                    query->addRule("logo", artwork.logoUrl);
                artworkPublished = true;
            }
        }
    }
    core_->printLn("SKR Gate: query rule %s=%s %s", protocol::kSkrQueryRule,
                   protocol::kSkrQueryRuleValue, skrPublished ? "published" : "NOT published");
    core_->printLn("SKR Gate: query rule allowed_clients=%s %s", advertised.c_str(),
                   clientsPublished ? "published" : "NOT published");
    core_->printLn("SKR Gate: query rule skr_mode=%s %s", clientPolicy_.ModeName(),
                   modePublished ? "published" : "NOT published");
    if (artwork.HasArtwork()) {
        core_->printLn("SKR Gate: artwork rules published (logo=%s, banner=%s)",
                       artwork.logoUrl.empty() ? "(none)" : "yes",
                       artwork.bannerUrl.empty() ? "(none)" : "yes");
    }

    core_->printLn("[SKR ClientPolicy] Mode: %s", clientPolicy_.ModeName());
    core_->printLn("[SKR ClientPolicy] SA-MP 0.3.7: %s", clientPolicy_.AllowsSamp037() ? "allowed" : "denied");
    core_->printLn("[SKR ClientPolicy] SKR Enhanced: %s", clientPolicy_.AllowsEnhanced() ? "allowed" : "denied");
    core_->printLn("[SKR ClientPolicy] open.mp Client: %s", clientPolicy_.AllowsOpenMp() ? "allowed" : "denied");
    core_->printLn("[SKR ClientPolicy] SA-MP 0.3.DL: %s", clientPolicy_.AllowsSamp03DL() ? "allowed" : "denied");
    core_->printLn("[SKR ClientPolicy] Enhanced negotiation timeout: %llu ms",
                   static_cast<unsigned long long>(clientPolicy_.EnhancedTimeoutMs()));
}

void SkrGateComponent::onFree(IComponent* component) {
    if (component == pawn_) pawn_ = nullptr;
}

void SkrGateComponent::free() {
    delete this;
}

void SkrGateComponent::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    players_.clear();
    generationCounter_ = 0;
}

// ── Player events ────────────────────────────────────────

AdmissionDecision SkrGateComponent::EvaluateClient(const IPlayer& player) const {
    // open.mp's own access/encryption handshake sets isUsingOmp(); this is not
    // derived from the arbitrary version-name string supplied in RPC 25.
    if (player.isUsingOmp() || player.getClientVersion() == ClientVersion::ClientVersion_openmp)
        return clientPolicy_.Evaluate(WireClientType::OpenMp);

    switch (player.getClientVersion()) {
        case ClientVersion::ClientVersion_SAMP_037:
            return clientPolicy_.Evaluate(WireClientType::Samp037);
        case ClientVersion::ClientVersion_SAMP_03DL:
            return clientPolicy_.Evaluate(WireClientType::Samp03DL);
        default:
            return clientPolicy_.Evaluate(WireClientType::Unsupported);
    }
}

void SkrGateComponent::RejectForClientPolicy(IPlayer& player, AdmissionDecision decision) {
    const protocol::RejectionReason reason = decision == AdmissionDecision::DenyUnsupported
        ? protocol::RejectionReason::UnsupportedClientVersion
        : protocol::RejectionReason::ClientVersionNotAllowed;
    core_->logLn(LogLevel::Warning, "[SKR ClientPolicy] Rejected player %d: %s",
                 player.getID(), AdmissionReason(decision));
    RejectWithReason(player, reason);
}

uint8_t SkrGateComponent::SupportedClientMask() const {
    uint8_t mask = 0;
    if (clientPolicy_.AllowsSamp037()) mask |= protocol::SupportedSamp037;
    if (clientPolicy_.AllowsEnhanced()) mask |= protocol::SupportedSkrEnhanced;
    if (clientPolicy_.AllowsOpenMp()) mask |= protocol::SupportedOpenMp;
    if (clientPolicy_.AllowsSamp03DL()) mask |= protocol::SupportedSamp03DL;
    return mask;
}

bool SkrGateComponent::SendEnhancedRejection(IPlayer& player, protocol::RejectionReason reason) {
    const protocol::RejectionMessage message {
        protocol::kEnhancedMarker,
        protocol::kEnhancedRejectionStep,
        protocol::kRejectionVersion,
        static_cast<uint8_t>(reason),
        SupportedClientMask(),
        {0, 0, 0}
    };
    NetworkBitStream bs;
    bs.Write(reinterpret_cast<const char*>(&message), sizeof(message));
    return player.sendRPC(protocol::kRpcSessionInfo,
        Span<uint8_t>(bs.GetData(), bs.GetNumberOfBitsUsed()), OrderingChannel_SyncRPC);
}

bool SkrGateComponent::SendLegacyRejection(IPlayer& player, protocol::RejectionReason reason) {
    const NewConnectionResult result =
        (reason == protocol::RejectionReason::UnsupportedClientVersion ||
         reason == protocol::RejectionReason::ClientVersionNotAllowed)
        ? NewConnectionResult_VersionMismatch : NewConnectionResult_BadMod;
    NetworkBitStream bs;
    bs.writeUINT8(static_cast<uint8_t>(result));
    return player.sendRPC(130, Span<uint8_t>(bs.GetData(), bs.GetNumberOfBitsUsed()),
                          OrderingChannel_SyncRPC);
}

void SkrGateComponent::RejectWithReason(IPlayer& player, protocol::RejectionReason reason,
                                         bool forceEnhancedNotice) {
    bool enhancedCapable = forceEnhancedNotice;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = players_.find(player.getID());
        if (it != players_.end()) {
            enhancedCapable = enhancedCapable ||
                it->second.enhanced.mode == protocol::ClientMode::EnhancedCandidate ||
                it->second.enhanced.mode == protocol::ClientMode::Enhanced;
            it->second.state = SkrPlayerState::Rejected;
        }
    }
    const bool enhancedSent = enhancedCapable && SendEnhancedRejection(player, reason);
    const bool legacySent = SendLegacyRejection(player, reason);
    core_->logLn(LogLevel::Warning,
        "[SKR Rejection] player=%d code=%s supported=\"%s\" enhanced_notice=%s legacy_notice=%s",
        player.getID(), protocol::RejectionCode(reason),
        protocol::SupportedClientsText(SupportedClientMask()).c_str(),
        enhancedSent ? "sent" : "unavailable", legacySent ? "sent" : "failed");
    player.kick();
}

bool SkrGateComponent::AdmitInitial(IPlayer& player) {
    const AdmissionDecision decision = EvaluateClient(player);
    if (decision == AdmissionDecision::Allow || decision == AdmissionDecision::RequireEnhanced)
        return true;
    RejectForClientPolicy(player, decision);
    return false;
}

void SkrGateComponent::onIncomingConnection(IPlayer& player, StringView ipAddress,
                                             unsigned short port) {
    int id = player.getID();
    const AdmissionDecision decision = EvaluateClient(player);
    if (decision != AdmissionDecision::Allow && decision != AdmissionDecision::RequireEnhanced) {
        RejectForClientPolicy(player, decision);
        return;
    }
    core_->printLn("SKR Gate: incoming connection from %.*s:%u (id %d)",
                   static_cast<int>(ipAddress.length()), ipAddress.data(), port, id);

    std::lock_guard<std::mutex> lock(mutex_);
    auto& data = players_[id];
    data.Reset();
    data.generation = ++generationCounter_;
    if (decision == AdmissionDecision::RequireEnhanced) {
        data.enhancedRequired = true;
        data.enhancedAdmissionDeadlineMs = NowMs() + clientPolicy_.EnhancedTimeoutMs();
    }
}

void SkrGateComponent::onPlayerConnect(IPlayer& player) {
    bool enhancedRequired = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = players_.find(player.getID());
        if (it != players_.end()) enhancedRequired = it->second.enhancedRequired;
    }
    core_->printLn("SKR Gate: player %d connected in %s mode (not authenticated)",
                   player.getID(), enhancedRequired ? "enhanced-pending" : clientPolicy_.ModeName());
}

void SkrGateComponent::onPlayerDisconnect(IPlayer& player, PeerDisconnectReason reason) {
    int id = player.getID();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        players_.erase(id);
    }
    clientStateHandler_.forget(id);
    radarRectHandler_.forget(id);
    core_->printLn("SKR Gate: player %d disconnected (reason %d)",
                   id, static_cast<int>(reason));
}

void SkrGateComponent::onPlayerClientInit(IPlayer& player) {
    int id = player.getID();
    core_->printLn("SKR Gate: player %d client initialized", id);
    if (IsEnhanced(id)) SendSessionInfo(player);
}

// True when the gamemode or a filterscript implements
// `public OnGameModeIntro()` and returns non-zero.
// A missing public returns the default (false): no server intro.
bool SkrGateComponent::ScriptsHaveIntro() {
    if (!pawn_) return false;
    if (IPawnScript* gm = pawn_->mainScript()) {
        if (gm->Call(protocol::kIntroCallback, DefaultReturnValue_False)) return true;
    }
    for (IPawnScript* fs : pawn_->sideScripts()) {
        if (fs && fs->Call(protocol::kIntroCallback, DefaultReturnValue_False)) return true;
    }
    return false;
}

// RPC 241 (shared/protocol/session_info.h). Runs after the scripts'
// OnPlayerConnect, so OnGameModeIntro can depend on what they set up there.
void SkrGateComponent::SendSessionInfo(IPlayer& player) {
    int id = player.getID();

    // Skip if already sent this session
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = players_.find(id);
        if (it == players_.end() || it->second.enhanced.mode != protocol::ClientMode::Enhanced
            || it->second.sessionInfoSent) return;
    }

    const uint8_t mode = static_cast<uint8_t>(ScriptsHaveIntro()
        ? protocol::SessionMode::CefIntro : protocol::SessionMode::None);

    NetworkBitStream bs;
    bs.writeUINT8(protocol::kSessionInfoVersion);
    bs.writeUINT8(mode);
    const bool sent = player.sendRPC(protocol::kRpcSessionInfo,
        Span<uint8_t>(bs.GetData(), bs.GetNumberOfBitsUsed()), OrderingChannel_SyncRPC);

    if (sent) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = players_.find(id);
        if (it != players_.end()) it->second.sessionInfoSent = true;
    }

    core_->printLn("SKR Gate: session info (mode %u) %s to player %d",
                   mode, sent ? "sent" : "NOT sent", id);

    // Radar state set by scripts before session info went out (e.g. in OnPlayerConnect).
    if (sent) SendRadarStyle(id);
}

// ── RPC 242: radar style/size (shared/protocol/radar_style.h) ──

bool SkrGateComponent::SetRadarStyle(int playerId, protocol::RadarStyle style) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = players_.find(playerId);
        if (it == players_.end() || it->second.state == SkrPlayerState::Rejected) return false;
        it->second.radarStyle = style;
    }
    SendRadarStyle(playerId);
    return true;
}

bool SkrGateComponent::SetRadarSize(int playerId, protocol::RadarSizeMode mode, float scale) {
    if (mode == protocol::RadarSizeMode::Default) {
        scale = 1.0f;
    } else if (!InRange(scale, protocol::kRadarMinScale, protocol::kRadarMaxScale)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = players_.find(playerId);
        if (it == players_.end() || it->second.state == SkrPlayerState::Rejected) return false;
        it->second.radarSizeMode = mode;
        it->second.radarScale = scale;
    }
    SendRadarStyle(playerId);
    return true;
}

void SkrGateComponent::SendRadarStyle(int playerId) {
    protocol::RadarStyle style;
    protocol::RadarSizeMode mode;
    float scale;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = players_.find(playerId);
        if (it == players_.end() || it->second.enhanced.mode != protocol::ClientMode::Enhanced
            || !it->second.sessionInfoSent) return;   // sent later from SendSessionInfo
        style = it->second.radarStyle;
        mode = it->second.radarSizeMode;
        scale = it->second.radarScale;
    }
    IPlayer* player = core_->getPlayers().get(playerId);
    if (!player) return;

    NetworkBitStream bs;
    bs.writeUINT8(protocol::kRadarStyleVersion);
    bs.writeUINT8(static_cast<uint8_t>(style));
    bs.writeUINT8(static_cast<uint8_t>(mode));
    bs.writeFLOAT(scale);
    const bool sent = player->sendRPC(protocol::kRpcRadarStyle,
        Span<uint8_t>(bs.GetData(), bs.GetNumberOfBitsUsed()), OrderingChannel_SyncRPC);
    core_->printLn("SKR Gate: radar style %u size %u scale %.2f %s to player %d",
                   static_cast<unsigned>(style), static_cast<unsigned>(mode), scale,
                   sent ? "sent" : "NOT sent", playerId);
}

// ── Tick: enforce auth timeout ───────────────────────────

void SkrGateComponent::onTick(Microseconds elapsed, TimePoint /*now*/) {
    tickAccumulatorMs_ += static_cast<uint64_t>(elapsed.count() / 1000);
    if (tickAccumulatorMs_ < 2000) return; // check every ~2 seconds
    tickAccumulatorMs_ = 0;

    uint64_t nowEpoch = static_cast<uint64_t>(std::time(nullptr));

    struct PendingKick { int id; protocol::RejectionReason reason; };
    std::vector<PendingKick> toKick;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [id, data] : players_) {
            if (data.enhancedRequired && data.enhanced.mode == protocol::ClientMode::Enhanced) {
                data.enhancedRequired = false;
                data.enhancedAdmissionDeadlineMs = 0;
            }
            if (data.enhancedRequired && data.enhancedAdmissionDeadlineMs > 0 &&
                NowMs() >= data.enhancedAdmissionDeadlineMs) {
                data.state = SkrPlayerState::Rejected;
                const auto reason = data.enhanced.mode == protocol::ClientMode::EnhancedCandidate
                    ? protocol::RejectionReason::EnhancedNegotiationTimeout
                    : protocol::RejectionReason::EnhancedRequired;
                toKick.push_back({id, reason});
                continue;
            }
            data.enhanced.Expire(NowMs());
            if (data.state == SkrPlayerState::Challenged &&
                data.challengeExpiresAt > 0 &&
                nowEpoch > data.challengeExpiresAt) {
                data.state = SkrPlayerState::Rejected;
                toKick.push_back({id, protocol::RejectionReason::AuthenticationFailed});
            }
        }
    }

    for (const PendingKick& pending : toKick) {
        auto* player = core_->getPlayers().get(pending.id);
        if (player) {
            RejectWithReason(*player, pending.reason);
        }
    }
}

// ── SKR gate logic ───────────────────────────────────────

// Display-only capabilities are independent of protected authorization.
bool SkrGateComponent::IsEnhanced(int id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = players_.find(id);
    return it != players_.end() && it->second.state != SkrPlayerState::Rejected &&
        it->second.enhanced.mode == protocol::ClientMode::Enhanced;
}

void SkrGateComponent::Negotiate(IPlayer& player, NetworkBitStream& bs) {
    if (bs.GetNumberOfBitsUsed() - bs.GetReadOffset() != 64) return;
    uint8_t marker = 0, step = 0, version = 0, caps = 0;
    uint32_t token = 0;
    if (!bs.readUINT8(marker) || !bs.readUINT8(step) || !bs.readUINT8(version) ||
        !bs.readUINT8(caps) || !bs.readUINT32(token) || marker != protocol::kEnhancedMarker) return;
    const bool validStep = step == static_cast<uint8_t>(protocol::EnhancedStep::Hello) ||
                           step == static_cast<uint8_t>(protocol::EnhancedStep::Confirm);
    if (validStep && !clientPolicy_.AllowsEnhanced()) {
        RejectWithReason(player, protocol::RejectionReason::ClientVersionNotAllowed,
                         true /* valid SKR Enhanced envelope received */);
        return;
    }
    bool offer = false, confirmed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = players_.find(player.getID());
        if (it == players_.end() || it->second.state == SkrPlayerState::Rejected) return;
        auto& session = it->second.enhanced;
        session.Expire(NowMs());
        if (step == static_cast<uint8_t>(protocol::EnhancedStep::Hello) && token == 0) {
            uint32_t generation = static_cast<uint32_t>(++generationCounter_);
            if (!generation) generation = static_cast<uint32_t>(++generationCounter_);
            offer = session.Begin(version, caps, generation, NowMs(), clientPolicy_.EnhancedTimeoutMs());
            token = session.token;
        } else if (step == static_cast<uint8_t>(protocol::EnhancedStep::Confirm)) {
            confirmed = session.Confirm(version, caps, token, NowMs());
            if (confirmed) {
                it->second.enhancedRequired = false;
                it->second.enhancedAdmissionDeadlineMs = 0;
            }
        }
    }
    if (offer) {
        NetworkBitStream response;
        response.writeUINT8(protocol::kEnhancedMarker);
        response.writeUINT8(static_cast<uint8_t>(protocol::EnhancedStep::Offer));
        response.writeUINT8(protocol::kEnhancedVersion);
        response.writeUINT8(protocol::kEnhancedCapabilities);
        response.writeUINT32(token);
        player.sendRPC(protocol::kRpcSessionInfo,
            Span<uint8_t>(response.GetData(), response.GetNumberOfBitsUsed()), OrderingChannel_SyncRPC);
    }
    if (confirmed) SendSessionInfo(player);
}

bool SkrGateComponent::IssueChallenge(int playerId,
                                       protocol::ChallengeMessage& challengeOut) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& data = players_[playerId];
    const protocol::EnhancedSession enhanced = data.enhanced;
    const bool enhancedRequired = data.enhancedRequired;
    const uint64_t enhancedDeadline = data.enhancedAdmissionDeadlineMs;
    data.Reset();
    data.enhanced = enhanced;
    data.enhancedRequired = enhancedRequired;
    data.enhancedAdmissionDeadlineMs = enhancedDeadline;
    data.generation = ++generationCounter_;

    crypto_.RandomBytes(data.connectionId, protocol::kConnectionIdSize);
    crypto_.RandomBytes(data.nonce, protocol::kNonceSize);

    uint64_t now = static_cast<uint64_t>(std::time(nullptr));
    data.challengeExpiresAt = now + protocol::kChallengeTimeoutSec;
    data.state = SkrPlayerState::Challenged;

    // Build challenge message
    challengeOut.version = protocol::kProtocolVersion;
    memset(challengeOut.server_id, 0, protocol::kMaxServerIdLen);
    memcpy(challengeOut.server_id, kServerId, kServerIdLen);
    challengeOut.server_id_len = kServerIdLen;
    memcpy(challengeOut.connection_id, data.connectionId, protocol::kConnectionIdSize);
    challengeOut.generation = data.generation;
    memcpy(challengeOut.nonce, data.nonce, protocol::kNonceSize);
    challengeOut.expires_at = data.challengeExpiresAt;

    return true;
}

bool SkrGateComponent::VerifyProof(int playerId,
                                    const protocol::ProofMessage& proof,
                                    const uint8_t publicKey[crypto::kKeySize]) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = players_.find(playerId);
    if (it == players_.end() || it->second.state != SkrPlayerState::Challenged)
        return false;

    auto& data = it->second;

    if (proof.version != protocol::kProtocolVersion)
        return false;

    uint64_t now = static_cast<uint64_t>(std::time(nullptr));
    if (now > data.challengeExpiresAt)
        return false;

    // Rebuild transcript
    protocol::ChallengeMessage challenge{};
    challenge.version = protocol::kProtocolVersion;
    memcpy(challenge.server_id, kServerId, kServerIdLen);
    challenge.server_id_len = kServerIdLen;
    memcpy(challenge.connection_id, data.connectionId, protocol::kConnectionIdSize);
    challenge.generation = data.generation;
    memcpy(challenge.nonce, data.nonce, protocol::kNonceSize);
    challenge.expires_at = data.challengeExpiresAt;

    protocol::TranscriptBuilder transcript;
    if (!transcript.Build(challenge, proof.join_id))
        return false;

    return crypto_.Verify(publicKey, transcript.buffer, transcript.length, proof.signature);
}

void SkrGateComponent::AuthorizePlayer(int playerId, uint32_t accountId) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = players_.find(playerId);
    if (it == players_.end()) return;

    it->second.state = SkrPlayerState::Authorized;
    it->second.accountId = accountId;

    if (core_) {
        core_->printLn("SKR Gate: player %d authorized (account %u)", playerId, accountId);
    }
}

void SkrGateComponent::RejectPlayer(int playerId, protocol::RejectionReason reason) {
    if (!protocol::IsValidRejectionReason(reason))
        reason = protocol::RejectionReason::AuthenticationFailed;
    IPlayer* player = core_ ? core_->getPlayers().get(playerId) : nullptr;
    if (player) RejectWithReason(*player, reason);
}

uint8_t SkrGateComponent::ProcessAcHeartbeat(int playerId,
                                              const protocol::AcHeartbeatMessage& beat,
                                              uint64_t nowMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = players_.find(playerId);
    if (it == players_.end() || it->second.state != SkrPlayerState::Authorized)
        return 2; // disconnect

    auto& data = it->second;

    if (beat.version != protocol::kProtocolVersion)
        return 1; // warn

    if (beat.sequence <= data.lastAcBeatSeq && data.lastAcBeatSeq > 0)
        return 1; // warn

    data.lastAcBeatSeq = beat.sequence;
    data.lastAcBeatMs = nowMs;
    data.missedAcBeats = 0;

    if (beat.acState == 4) // Failed
        return 2;

    if (beat.acVerdict == 2) // Detected
        return 1;

    return 0; // continue
}

const SkrPlayerData* SkrGateComponent::GetPlayerData(int playerId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = players_.find(playerId);
    return it != players_.end() ? &it->second : nullptr;
}

bool SkrGateComponent::IsAuthorized(int playerId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = players_.find(playerId);
    return it != players_.end() && it->second.state == SkrPlayerState::Authorized;
}

} // namespace skr::gate_omp

// ── Entry point ──────────────────────────────────────────

COMPONENT_ENTRY_POINT() {
    return new skr::gate_omp::SkrGateComponent();
}
