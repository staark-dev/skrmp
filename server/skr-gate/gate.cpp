#include "gate.h"
#include <cstring>
#include <ctime>

namespace skr::gate {

SkrGate::SkrGate(crypto::ISigningProvider& crypto)
    : crypto_(crypto) {
    const char* id = "skr-main";
    serverIdLen_ = static_cast<uint8_t>(strlen(id));
    memcpy(serverId_, id, serverIdLen_);
}

bool SkrGate::OnPlayerConnect(uint16_t playerId,
                               const char* /*serverId*/,
                               protocol::ChallengeMessage& challengeOut) {
    if (playerId >= kMaxPlayers) return false;

    auto& session = sessions_[playerId];
    session.Reset();
    session.state = protocol::GateState::Quarantined;
    session.generation = ++generationCounter_;
    session.stateChanged = SteadyClock::now();

    // Generate connection ID and nonce
    crypto_.RandomBytes(session.connectionId, protocol::kConnectionIdSize);
    crypto_.RandomBytes(session.nonce, protocol::kNonceSize);

    // Set expiry
    uint64_t now = static_cast<uint64_t>(std::time(nullptr));
    session.expiresAt = now + protocol::kChallengeTimeoutSec;

    // Build challenge message
    challengeOut.version = protocol::kProtocolVersion;
    memset(challengeOut.server_id, 0, protocol::kMaxServerIdLen);
    memcpy(challengeOut.server_id, serverId_, serverIdLen_);
    challengeOut.server_id_len = serverIdLen_;
    memcpy(challengeOut.connection_id, session.connectionId, protocol::kConnectionIdSize);
    challengeOut.generation = session.generation;
    memcpy(challengeOut.nonce, session.nonce, protocol::kNonceSize);
    challengeOut.expires_at = session.expiresAt;

    session.state = protocol::GateState::Challenged;
    return true;
}

bool SkrGate::OnProofReceived(uint16_t playerId,
                               const protocol::ProofMessage& proof,
                               const uint8_t publicKey[crypto::kKeySize]) {
    if (playerId >= kMaxPlayers) return false;
    auto& session = sessions_[playerId];

    if (session.state != protocol::GateState::Challenged) {
        Reject(playerId, protocol::AuthError::AuthRequired);
        return false;
    }

    if (proof.version != protocol::kProtocolVersion) {
        Reject(playerId, protocol::AuthError::ProtocolUnsupported);
        return false;
    }

    // Check deadline
    uint64_t now = static_cast<uint64_t>(std::time(nullptr));
    if (now > session.expiresAt) {
        Reject(playerId, protocol::AuthError::TicketExpired);
        return false;
    }

    // Rebuild transcript and verify signature
    protocol::ChallengeMessage challenge{};
    challenge.version = protocol::kProtocolVersion;
    memcpy(challenge.server_id, serverId_, serverIdLen_);
    challenge.server_id_len = serverIdLen_;
    memcpy(challenge.connection_id, session.connectionId, protocol::kConnectionIdSize);
    challenge.generation = session.generation;
    memcpy(challenge.nonce, session.nonce, protocol::kNonceSize);
    challenge.expires_at = session.expiresAt;

    protocol::TranscriptBuilder transcript;
    if (!transcript.Build(challenge, proof.join_id)) {
        Reject(playerId, protocol::AuthError::InternalError);
        return false;
    }

    if (!crypto_.Verify(publicKey, transcript.buffer, transcript.length, proof.signature)) {
        Reject(playerId, protocol::AuthError::InvalidSignature);
        return false;
    }

    session.state = protocol::GateState::Verifying;
    session.stateChanged = SteadyClock::now();
    // Caller must now asynchronously redeem the ticket with the backend.
    return true;
}

void SkrGate::OnRedeemResult(uint16_t playerId, bool valid, uint32_t accountId) {
    if (playerId >= kMaxPlayers) return;
    auto& session = sessions_[playerId];

    if (session.state != protocol::GateState::Verifying) return;

    if (valid) {
        session.state = protocol::GateState::Authorized;
        session.accountId = accountId;
        session.gameplayAllowed = true;
        session.stateChanged = SteadyClock::now();
    } else {
        Reject(playerId, protocol::AuthError::TicketUsed);
    }
}

uint8_t SkrGate::OnAcHeartbeat(uint16_t playerId,
                                const protocol::AcHeartbeatMessage& beat,
                                uint64_t nowMs) {
    if (playerId >= kMaxPlayers) return 2;
    auto& session = sessions_[playerId];

    if (session.state != protocol::GateState::Authorized) return 2;

    if (beat.version != protocol::kProtocolVersion) return 1;

    // Sequence must advance
    if (beat.sequence <= session.lastAcBeatSeq && session.lastAcBeatSeq > 0)
        return 1;

    session.lastAcBeatSeq = beat.sequence;
    session.lastAcBeatMs = nowMs;
    session.missedAcBeats = 0;
    session.lastAcState = beat.acState;
    session.lastAcVerdict = beat.acVerdict;

    // AC reported failure → disconnect
    if (beat.acState == 4) { // AcState::Failed
        Reject(playerId, protocol::AuthError::AcRequired);
        return 2;
    }

    // AC detected something → warn (server decides separately on ban)
    if (beat.acVerdict == 2) // AcVerdict::Detected
        return 1;

    return 0; // continue
}

void SkrGate::Tick(uint64_t nowUnix) {
    uint64_t nowMs = nowUnix * 1000;

    for (uint16_t i = 0; i < kMaxPlayers; ++i) {
        auto& s = sessions_[i];

        // Expire pending auth
        if (s.state == protocol::GateState::Challenged ||
            s.state == protocol::GateState::Verifying) {
            if (nowUnix > s.expiresAt + protocol::kVerifyTimeoutSec) {
                Reject(i, protocol::AuthError::TicketExpired);
            }
        }

        // Check AC heartbeat timeout for authorized players
        if (s.state == protocol::GateState::Authorized && s.lastAcBeatMs > 0) {
            if (nowMs - s.lastAcBeatMs > protocol::kAcHeartbeatTimeoutMs) {
                ++s.missedAcBeats;
                if (s.missedAcBeats >= protocol::kAcMaxMissedBeats) {
                    Reject(i, protocol::AuthError::AcRequired);
                }
            }
        }
    }
}

const PlayerSession* SkrGate::GetSession(uint16_t playerId) const {
    if (playerId >= kMaxPlayers) return nullptr;
    return &sessions_[playerId];
}

bool SkrGate::IsAuthorized(uint16_t playerId) const {
    if (playerId >= kMaxPlayers) return false;
    return sessions_[playerId].state == protocol::GateState::Authorized;
}

void SkrGate::OnPlayerDisconnect(uint16_t playerId) {
    if (playerId >= kMaxPlayers) return;
    sessions_[playerId].Reset();
}

void SkrGate::Reject(uint16_t playerId, protocol::AuthError reason) {
    if (playerId >= kMaxPlayers) return;
    auto& session = sessions_[playerId];
    session.state = protocol::GateState::Rejected;
    session.gameplayAllowed = false;
    session.stateChanged = SteadyClock::now();
    // TODO: send ResultMessage to player and disconnect after grace period
    (void)reason;
}

} // namespace skr::gate
