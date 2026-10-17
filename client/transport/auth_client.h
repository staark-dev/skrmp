#pragma once
#include "protocol/messages.h"
#include "crypto/crypto.h"
#include <cstdint>

namespace skr::transport {

enum class AuthState : uint8_t {
    Idle,
    WaitingForTicket,   // IPC request sent to launcher
    TicketReady,        // ticket received, waiting for game connect
    Challenged,         // received challenge from server
    ProofSent,          // signed proof sent, awaiting result
    Authorized,
    Failed
};

class AuthClient {
public:
    explicit AuthClient(crypto::ISigningProvider& crypto);

    // Called once after DLL init to generate ephemeral key pair.
    bool Initialize();

    // Called when launcher provides ticket via IPC.
    void OnTicketReceived(const uint8_t joinId[protocol::kJoinIdSize]);

    // Called when server sends challenge.
    bool OnChallenge(const protocol::ChallengeMessage& challenge,
                     protocol::ProofMessage& proofOut);

    // Called when server sends result.
    void OnResult(const protocol::ResultMessage& result);

    AuthState State() const { return state_; }
    const uint8_t* PublicKey() const { return keyPair_.publicKey; }
    protocol::AuthError LastError() const { return lastError_; }

private:
    crypto::ISigningProvider& crypto_;
    crypto::KeyPair keyPair_{};
    uint8_t joinId_[protocol::kJoinIdSize]{};
    AuthState state_ = AuthState::Idle;
    protocol::AuthError lastError_ = protocol::AuthError::None;
    bool initialized_ = false;
};

} // namespace skr::transport
