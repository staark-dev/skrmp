#pragma once
#include "protocol/messages.h"
#include "crypto/crypto.h"
#include <cstdint>
#include <cstring>
#include <chrono>

namespace skr::gate {

using SteadyClock = std::chrono::steady_clock;
using TimePoint = SteadyClock::time_point;

struct PlayerSession {
    protocol::GateState state = protocol::GateState::Connected;
    uint8_t connectionId[protocol::kConnectionIdSize]{};
    uint64_t generation = 0;
    uint8_t nonce[protocol::kNonceSize]{};
    uint64_t expiresAt = 0;       // UNIX seconds for challenge deadline
    TimePoint stateChanged;
    uint32_t accountId = 0;       // set after authorization
    bool gameplayAllowed = false;

    // AC heartbeat tracking
    uint32_t lastAcBeatSeq = 0;
    uint64_t lastAcBeatMs = 0;
    uint32_t missedAcBeats = 0;
    uint8_t  lastAcState = 0;     // AcState from client
    uint8_t  lastAcVerdict = 0;   // AcVerdict from client

    void Reset() {
        state = protocol::GateState::Connected;
        generation = 0;
        expiresAt = 0;
        accountId = 0;
        gameplayAllowed = false;
        lastAcBeatSeq = 0;
        lastAcBeatMs = 0;
        missedAcBeats = 0;
        lastAcState = 0;
        lastAcVerdict = 0;
        memset(nonce, 0, protocol::kNonceSize);
    }
};

class SkrGate {
public:
    explicit SkrGate(crypto::ISigningProvider& crypto);

    // Called when a player connects. Moves to Quarantined and issues challenge.
    bool OnPlayerConnect(uint16_t playerId,
                         const char* serverId,
                         protocol::ChallengeMessage& challengeOut);

    // Called when player sends proof. Validates and moves to Verifying.
    // Returns true if proof is well-formed; actual ticket verification is async.
    bool OnProofReceived(uint16_t playerId,
                         const protocol::ProofMessage& proof,
                         const uint8_t publicKey[crypto::kKeySize]);

    // Called with backend redeem result. Moves to Authorized or Rejected.
    void OnRedeemResult(uint16_t playerId, bool valid, uint32_t accountId);

    // Called periodically to expire timed-out sessions.
    void Tick(uint64_t nowUnix);

    // Query state
    const PlayerSession* GetSession(uint16_t playerId) const;
    bool IsAuthorized(uint16_t playerId) const;

    // Process AC heartbeat from authorized client.
    // Returns action: 0 = continue, 1 = warn, 2 = disconnect.
    uint8_t OnAcHeartbeat(uint16_t playerId,
                          const protocol::AcHeartbeatMessage& beat,
                          uint64_t nowMs);

    // Called on disconnect.
    void OnPlayerDisconnect(uint16_t playerId);

    static constexpr uint16_t kMaxPlayers = 1000;

private:
    void Reject(uint16_t playerId, protocol::AuthError reason);

    crypto::ISigningProvider& crypto_;
    PlayerSession sessions_[kMaxPlayers];
    uint64_t generationCounter_ = 0;
    char serverId_[protocol::kMaxServerIdLen]{};
    uint8_t serverIdLen_ = 0;
};

} // namespace skr::gate
