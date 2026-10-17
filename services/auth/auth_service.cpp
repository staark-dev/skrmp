#include "auth_service.h"
#include <cstring>
#include <ctime>
#include <cstdio>

namespace skr::auth {

AuthService::AuthService(crypto::ISigningProvider& crypto)
    : crypto_(crypto) {}

std::string AuthService::HashToHex(const uint8_t hash[32]) {
    char hex[65];
    for (int i = 0; i < 32; ++i)
        snprintf(hex + i * 2, 3, "%02x", hash[i]);
    hex[64] = '\0';
    return std::string(hex);
}

void AuthService::HashJoinId(const uint8_t joinId[protocol::kJoinIdSize], uint8_t out[32]) {
    // TODO: replace with real SHA-256 (Windows CNG BCryptHash or a library).
    // Mock: XOR-fold the join_id into 32 bytes (NOT cryptographic).
    memset(out, 0, 32);
    for (size_t i = 0; i < protocol::kJoinIdSize; ++i)
        out[i % 32] ^= joinId[i];
}

bool AuthService::IssueTicket(uint32_t accountId,
                               const uint8_t publicKey[crypto::kKeySize],
                               const char* serverId,
                               const char* clientBuild,
                               uint8_t joinIdOut[protocol::kJoinIdSize]) {
    if (!serverId) return false;

    // Generate random join_id
    if (!crypto_.RandomBytes(joinIdOut, protocol::kJoinIdSize))
        return false;

    TicketRecord record;
    HashJoinId(joinIdOut, record.joinIdHash);
    record.accountId = accountId;
    memcpy(record.publicKey, publicKey, crypto::kKeySize);
    record.serverId = serverId;
    record.expiresAt = static_cast<uint64_t>(std::time(nullptr))
                       + protocol::kTicketLifetimeSec;
    record.redeemed = false;
    record.clientBuild = clientBuild ? clientBuild : "";

    std::string key = HashToHex(record.joinIdHash);

    std::lock_guard<std::mutex> lock(mutex_);
    tickets_[key] = std::move(record);
    return true;
}

uint32_t AuthService::RedeemTicket(const uint8_t joinId[protocol::kJoinIdSize],
                                    const char* serverId,
                                    protocol::AuthError& errorOut) {
    uint8_t hash[32];
    HashJoinId(joinId, hash);
    std::string key = HashToHex(hash);

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tickets_.find(key);
    if (it == tickets_.end()) {
        errorOut = protocol::AuthError::AuthRequired;
        return 0;
    }

    auto& record = it->second;

    if (record.redeemed) {
        errorOut = protocol::AuthError::TicketUsed;
        return 0;
    }

    uint64_t now = static_cast<uint64_t>(std::time(nullptr));
    if (now > record.expiresAt) {
        errorOut = protocol::AuthError::TicketExpired;
        return 0;
    }

    if (serverId && record.serverId != serverId) {
        errorOut = protocol::AuthError::ConnectionMismatch;
        return 0;
    }

    // Atomic consume
    record.redeemed = true;
    errorOut = protocol::AuthError::None;
    return record.accountId;
}

void AuthService::RevokeAccount(uint32_t accountId) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = tickets_.begin(); it != tickets_.end();) {
        if (it->second.accountId == accountId)
            it = tickets_.erase(it);
        else
            ++it;
    }
}

} // namespace skr::auth
