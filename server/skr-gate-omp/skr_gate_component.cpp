#include "skr_gate_component.hpp"
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
    if (!gate_ || !gate_->IsAuthorized(id)) return false;

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

bool SkrJoinFilter::onReceive(IPlayer& peer, NetworkBitStream& bs) {
    int savedOffset = bs.GetReadOffset();
    int totalBits = bs.GetNumberOfBitsUsed();

    if (core_)
        core_->printLn("SKR Gate RPC25: totalBits=%d readOffset=%d", totalBits, savedOffset);

    // CLIENT_JOIN structure: version(int32), mod(uint8),
    // nickname(uint8 len + chars), challenge(uint32),
    // authKey(uint8 len + chars), clientVer(uint8 len + chars)
    int32_t version = 0;
    uint8_t mod = 0, nickLen = 0;
    if (!bs.readINT32(version) || !bs.readUINT8(mod) || !bs.readUINT8(nickLen)) {
        if (core_) core_->printLn("SKR Gate RPC25: failed reading header");
        bs.SetReadOffset(savedOffset);
        return true;
    }

    if (core_)
        core_->printLn("SKR Gate RPC25: ver=%d mod=%d nickLen=%d", version, mod, nickLen);

    // Skip nickname bytes
    bs.SetReadOffset(bs.GetReadOffset() + nickLen * 8);

    uint32_t challenge = 0;
    if (!bs.readUINT32(challenge)) {
        if (core_) core_->printLn("SKR Gate RPC25: failed reading challenge");
        bs.SetReadOffset(savedOffset); return true;
    }

    // Skip auth key (uint8 len + chars)
    uint8_t authLen = 0;
    if (!bs.readUINT8(authLen)) {
        if (core_) core_->printLn("SKR Gate RPC25: failed reading authLen");
        bs.SetReadOffset(savedOffset); return true;
    }

    if (core_)
        core_->printLn("SKR Gate RPC25: challenge=0x%X authLen=%d", challenge, authLen);

    bs.SetReadOffset(bs.GetReadOffset() + authLen * 8);

    // Read client version string
    uint8_t verLen = 0;
    if (!bs.readUINT8(verLen)) {
        if (core_) core_->printLn("SKR Gate RPC25: failed reading verLen");
        bs.SetReadOffset(savedOffset); return true;
    }

    if (core_)
        core_->printLn("SKR Gate RPC25: verLen=%d bitsLeft=%d",
                       verLen, totalBits - bs.GetReadOffset());

    char verStr[65] = {};
    if (verLen > 0 && verLen <= 64) {
        if (!bs.Read(verStr, verLen)) {
            if (core_) core_->printLn("SKR Gate RPC25: failed reading verStr");
            bs.SetReadOffset(savedOffset); return true;
        }
    }
    verStr[verLen] = '\0';

    bs.SetReadOffset(savedOffset);

    bool isSKR = (verLen >= 3 && verStr[0] == 'S' && verStr[1] == 'K' && verStr[2] == 'R');

    if (core_)
        core_->printLn("SKR Gate RPC25: clientVer='%s' (len=%d) %s",
                       verStr, verLen, isSKR ? "[ALLOWED]" : "[BLOCKED]");

    if (!isSKR) {
        peer.sendClientMessage(Colour::FromRGBA(0xA9C4E4FF),
            "Checking client compatibility...");
        peer.sendClientMessage(Colour::FromRGBA(0xAA3333FF),
            "Connection rejected: SKR RC1 Client required.");
        peer.sendClientMessage(Colour::FromRGBA(0xA9C4E4FF),
            "Please download from skr-server.com to join.");
        peer.kick();
        return false;
    }

    return true;
}

SkrGateComponent::~SkrGateComponent() {
    if (core_) {
        core_->removePerRPCInEventHandler<kRPC_ClientJoin>(&joinFilter_);
        core_->removePerRPCInEventHandler<protocol::kRpcClientState>(&clientStateHandler_);
        core_->getEventDispatcher().removeEventHandler(this);
        core_->getPlayers().getPlayerConnectDispatcher().removeEventHandler(this);
    }
}

// ── IComponent lifecycle ─────────────────────────────────

void SkrGateComponent::onLoad(ICore* c) {
    core_ = c;
    core_->printLn("SKR Gate component loaded (v0.1.0)");
}

void SkrGateComponent::onInit(IComponentList* components) {
    pawn_ = components->queryComponent<IPawnComponent>();
    core_->getPlayers().getPlayerConnectDispatcher().addEventHandler(this);
    core_->getEventDispatcher().addEventHandler(this);

    // Register per-RPC handler for CLIENT_JOIN (RPC 25) — high priority to run first
    joinFilter_.setCore(core_);
    core_->addPerRPCInEventHandler<kRPC_ClientJoin>(&joinFilter_, EventPriority_FairlyHigh);

    clientStateHandler_.setOwner(this, core_);
    core_->addPerRPCInEventHandler<protocol::kRpcClientState>(&clientStateHandler_);
    core_->printLn("SKR Gate: registered player event handlers + RPC filter + client-state RPC %d",
                   protocol::kRpcClientState);
}

void SkrGateComponent::onReady() {
    core_->printLn("SKR Gate: ready, enforcing ticket-based auth");

    // Advertise native SKR support in the query rules (shared/protocol/query_rules.h).
    bool published = false;
    for (INetwork* network : core_->getNetworks()) {
        if (auto* query = queryExtension<INetworkQueryExtension>(network)) {
            published = query->addRule(protocol::kSkrQueryRule, protocol::kSkrQueryRuleValue) || published;
        }
    }
    core_->printLn("SKR Gate: query rule %s=%s %s", protocol::kSkrQueryRule,
                   protocol::kSkrQueryRuleValue, published ? "published" : "NOT published");
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

void SkrGateComponent::onIncomingConnection(IPlayer& player, StringView ipAddress,
                                             unsigned short port) {
    int id = player.getID();
    core_->printLn("SKR Gate: incoming connection from %.*s:%u (id %d)",
                   static_cast<int>(ipAddress.length()), ipAddress.data(), port, id);

    player.sendClientMessage(Colour::FromRGBA(0xA9C4E4FF),
        "Checking client compatibility...");

    // Issue challenge
    protocol::ChallengeMessage challenge{};
    if (IssueChallenge(id, challenge)) {
        core_->printLn("SKR Gate: challenge issued to player %d (gen %llu)",
                       id, challenge.generation);
    }
}

void SkrGateComponent::onPlayerConnect(IPlayer& player) {
    int id = player.getID();
    core_->printLn("SKR Gate: player %d fully connected", id);

    // If player got past the RPC filter, they're SKR — authorize
    player.sendClientMessage(Colour::FromRGBA(0xA9C4E4FF),
        "SKR Client verified.");
    player.sendClientMessage(Colour::FromRGBA(0xD4A017FF),
        "Authenticating... Please wait.");
    // TODO: verify launcher token here before calling AuthorizePlayer
    AuthorizePlayer(id, 0);

    // Send session info early so the client knows about CefIntro before
    // CEF initialization — the lobby can keep showing while auth loads.
    SendSessionInfo(player);
}

void SkrGateComponent::onPlayerDisconnect(IPlayer& player, PeerDisconnectReason reason) {
    int id = player.getID();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        players_.erase(id);
    }
    clientStateHandler_.forget(id);
    core_->printLn("SKR Gate: player %d disconnected (reason %d)",
                   id, static_cast<int>(reason));
}

void SkrGateComponent::onPlayerClientInit(IPlayer& player) {
    int id = player.getID();
    core_->printLn("SKR Gate: player %d client initialized", id);
    if (IsAuthorized(id)) SendSessionInfo(player);
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
        if (it != players_.end() && it->second.sessionInfoSent) return;
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
}

// ── Tick: enforce auth timeout ───────────────────────────

void SkrGateComponent::onTick(Microseconds elapsed, TimePoint /*now*/) {
    tickAccumulatorMs_ += static_cast<uint64_t>(elapsed.count() / 1000);
    if (tickAccumulatorMs_ < 2000) return; // check every ~2 seconds
    tickAccumulatorMs_ = 0;

    uint64_t nowEpoch = static_cast<uint64_t>(std::time(nullptr));

    std::vector<int> toKick;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [id, data] : players_) {
            if (data.state == SkrPlayerState::Challenged &&
                data.challengeExpiresAt > 0 &&
                nowEpoch > data.challengeExpiresAt) {
                data.state = SkrPlayerState::Rejected;
                toKick.push_back(id);
            }
        }
    }

    for (int id : toKick) {
        auto* player = core_->getPlayers().get(id);
        if (player) {
            core_->printLn("SKR Gate: kicking player %d (auth timeout)", id);
            player->sendClientMessage(Colour::FromRGBA(0xFF4444FF),
                "Connection rejected: SKR Client required.");
            player->kick();
        }
    }
}

// ── SKR gate logic ───────────────────────────────────────

bool SkrGateComponent::IssueChallenge(int playerId,
                                       protocol::ChallengeMessage& challengeOut) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& data = players_[playerId];
    data.Reset();
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

void SkrGateComponent::RejectPlayer(int playerId, const char* reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = players_.find(playerId);
    if (it == players_.end()) return;

    it->second.state = SkrPlayerState::Rejected;

    if (core_) {
        core_->printLn("SKR Gate: player %d rejected (%s)", playerId,
                       reason ? reason : "unknown");

        // Send reject message and kick
        auto* player = core_->getPlayers().get(playerId);
        if (player) {
            player->sendClientMessage(Colour::FromRGBA(0xFF0000FF),
                "This server requires SKR Client.");
            player->kick();
        }
    }
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
