#pragma once
#include "protocol/messages.h"
#include "crypto/crypto.h"
#include <cstdint>
#include <unordered_map>
#include <mutex>
#include <string>

namespace skr::auth {

struct TicketRecord {
    uint8_t joinIdHash[32]{};        // SHA-256 of the join_id
    uint32_t accountId = 0;
    uint8_t publicKey[crypto::kKeySize]{};
    std::string serverId;
    uint64_t expiresAt = 0;          // UNIX seconds
    bool redeemed = false;
    std::string clientBuild;
    std::string compatProfile;
};

// In-memory auth service for development. Production uses a database.
class AuthService {
public:
    explicit AuthService(crypto::ISigningProvider& crypto);

    // Issue a join ticket. Returns the raw join_id (to be sent to client).
    // Stores a hashed record internally.
    bool IssueTicket(uint32_t accountId,
                     const uint8_t publicKey[crypto::kKeySize],
                     const char* serverId,
                     const char* clientBuild,
                     uint8_t joinIdOut[protocol::kJoinIdSize]);

    // Redeem (consume) a ticket. Atomic: two concurrent calls for the same
    // ticket must result in exactly one success.
    // Returns the account ID on success, 0 on failure.
    uint32_t RedeemTicket(const uint8_t joinId[protocol::kJoinIdSize],
                          const char* serverId,
                          protocol::AuthError& errorOut);

    // Revoke all tickets for an account.
    void RevokeAccount(uint32_t accountId);

private:
    crypto::ISigningProvider& crypto_;
    std::mutex mutex_;
    // Key: hex string of joinIdHash
    std::unordered_map<std::string, TicketRecord> tickets_;

    static std::string HashToHex(const uint8_t hash[32]);
    void HashJoinId(const uint8_t joinId[protocol::kJoinIdSize], uint8_t out[32]);
};

} // namespace skr::auth
