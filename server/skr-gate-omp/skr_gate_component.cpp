#include "skr_gate_component.hpp"
#include <cstdio>
#include <vector>

namespace skr::gate_omp {

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
        core_->getEventDispatcher().removeEventHandler(this);
        core_->getPlayers().getPlayerConnectDispatcher().removeEventHandler(this);
    }
}

// ── IComponent lifecycle ─────────────────────────────────

void SkrGateComponent::onLoad(ICore* c) {
    core_ = c;
    core_->printLn("SKR Gate component loaded (v0.1.0)");
}

void SkrGateComponent::onInit(IComponentList* /*components*/) {
    core_->getPlayers().getPlayerConnectDispatcher().addEventHandler(this);
    core_->getEventDispatcher().addEventHandler(this);

    // Register per-RPC handler for CLIENT_JOIN (RPC 25) — high priority to run first
    joinFilter_.setCore(core_);
    core_->addPerRPCInEventHandler<kRPC_ClientJoin>(&joinFilter_, EventPriority_FairlyHigh);
    core_->printLn("SKR Gate: registered player event handlers + RPC filter");
}

void SkrGateComponent::onReady() {
    core_->printLn("SKR Gate: ready, enforcing ticket-based auth");
}

void SkrGateComponent::onFree(IComponent* /*component*/) {
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
}

void SkrGateComponent::onPlayerDisconnect(IPlayer& player, PeerDisconnectReason reason) {
    int id = player.getID();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        players_.erase(id);
    }
    core_->printLn("SKR Gate: player %d disconnected (reason %d)",
                   id, static_cast<int>(reason));
}

void SkrGateComponent::onPlayerClientInit(IPlayer& player) {
    int id = player.getID();
    core_->printLn("SKR Gate: player %d client initialized", id);
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
