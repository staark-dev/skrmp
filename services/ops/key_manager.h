#pragma once
#include "crypto/crypto.h"
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>
#include <string>

namespace skr::ops {

constexpr size_t kMaxKeyId = 64;
constexpr size_t kMaxKeys = 8;

enum class KeyState : uint8_t {
    Active    = 0,
    Rotating  = 1,
    Revoked   = 2,
    Expired   = 3
};

enum class KeyPurpose : uint8_t {
    ManifestSigning = 0,
    TicketSigning   = 1,
    SessionSigning  = 2
};

struct ManagedKey {
    char keyId[kMaxKeyId]{};
    KeyPurpose purpose = KeyPurpose::ManifestSigning;
    KeyState state = KeyState::Active;
    uint8_t publicKey[crypto::kKeySize]{};
    uint64_t createdAt = 0;
    uint64_t expiresAt = 0;
    uint64_t revokedAt = 0;
};

struct RotationResult {
    bool success = false;
    char oldKeyId[kMaxKeyId]{};
    char newKeyId[kMaxKeyId]{};
    char error[128]{};
};

class KeyManager {
public:
    explicit KeyManager(crypto::ISigningProvider& crypto);

    bool AddKey(const char* keyId, KeyPurpose purpose,
                const uint8_t publicKey[crypto::kKeySize],
                uint64_t expiresAt);

    bool RevokeKey(const char* keyId, uint64_t nowTimestamp);

    RotationResult RotateKey(const char* oldKeyId, uint64_t nowTimestamp);

    const ManagedKey* GetActiveKey(KeyPurpose purpose) const;
    const ManagedKey* FindKey(const char* keyId) const;

    bool IsKeyValid(const char* keyId, uint64_t nowTimestamp) const;

    void ExpireOldKeys(uint64_t nowTimestamp);

    // Emergency: revoke all keys and suspend issuance
    void RevokeAll(uint64_t nowTimestamp);
    bool IsIssuanceSuspended() const { return issuanceSuspended_; }
    void ResumeIssuance() { issuanceSuspended_ = false; }

    const std::vector<ManagedKey>& GetAllKeys() const { return keys_; }

private:
    crypto::ISigningProvider& crypto_;
    mutable std::mutex mutex_;
    std::vector<ManagedKey> keys_;
    bool issuanceSuspended_ = false;
};

} // namespace skr::ops
