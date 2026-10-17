#include "skr_gate_component.hpp"
#include <cstdio>

namespace skr::gate_omp {

SkrGateComponent::~SkrGateComponent() {
    if (core_) {
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
    core_->printLn("SKR Gate: registered player event handlers");
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

    // Issue challenge immediately
    protocol::ChallengeMessage challenge{};
    if (IssueChallenge(id, challenge)) {
        core_->printLn("SKR Gate: challenge issued to player %d (gen %llu)",
                       id, challenge.generation);
        // TODO: send challenge via RPC to client
        // player.sendRPC(...) — requires custom RPC ID registration
    }
}

void SkrGateComponent::onPlayerConnect(IPlayer& player) {
    int id = player.getID();
    core_->printLn("SKR Gate: player %d fully connected", id);

    // If not yet authorized, send a message and start a timeout
    if (!IsAuthorized(id)) {
        player.sendClientMessage(Colour::Yellow(),
            "Authenticating... Please wait.");
    }
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
