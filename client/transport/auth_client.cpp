#include "transport/auth_client.h"
#include <cstring>

namespace skr::transport {

AuthClient::AuthClient(crypto::ISigningProvider& crypto)
    : crypto_(crypto) {}

bool AuthClient::Initialize() {
    if (initialized_) return true;
    if (!crypto_.GenerateKeyPair(keyPair_)) return false;
    initialized_ = true;
    state_ = AuthState::Idle;
    return true;
}

void AuthClient::OnTicketReceived(const uint8_t joinId[protocol::kJoinIdSize]) {
    memcpy(joinId_, joinId, protocol::kJoinIdSize);
    state_ = AuthState::TicketReady;
    lastError_ = protocol::AuthError::None;
}

bool AuthClient::OnChallenge(const protocol::ChallengeMessage& challenge,
                             protocol::ProofMessage& proofOut) {
    if (state_ != AuthState::TicketReady) {
        lastError_ = protocol::AuthError::AuthRequired;
        state_ = AuthState::Failed;
        return false;
    }

    if (challenge.version != protocol::kProtocolVersion) {
        lastError_ = protocol::AuthError::ProtocolUnsupported;
        state_ = AuthState::Failed;
        return false;
    }

    // Build canonical transcript
    protocol::TranscriptBuilder transcript;
    if (!transcript.Build(challenge, joinId_)) {
        lastError_ = protocol::AuthError::InternalError;
        state_ = AuthState::Failed;
        return false;
    }

    // Sign transcript with ephemeral private key
    proofOut.version = protocol::kProtocolVersion;
    memcpy(proofOut.join_id, joinId_, protocol::kJoinIdSize);

    if (!crypto_.Sign(keyPair_.privateKey,
                      transcript.buffer, transcript.length,
                      proofOut.signature)) {
        lastError_ = protocol::AuthError::InternalError;
        state_ = AuthState::Failed;
        return false;
    }

    state_ = AuthState::ProofSent;
    return true;
}

void AuthClient::OnResult(const protocol::ResultMessage& result) {
    if (result.status == 1) {
        state_ = AuthState::Authorized;
        lastError_ = protocol::AuthError::None;
    } else {
        state_ = AuthState::Failed;
        lastError_ = static_cast<protocol::AuthError>(result.error_code);
    }
}

} // namespace skr::transport
