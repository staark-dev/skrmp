#include "key_manager.h"
#include <cstdio>
#include <ctime>

namespace skr::ops {

KeyManager::KeyManager(crypto::ISigningProvider& crypto)
    : crypto_(crypto) {}

bool KeyManager::AddKey(const char* keyId, KeyPurpose purpose,
                        const uint8_t publicKey[crypto::kKeySize],
                        uint64_t expiresAt) {
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& k : keys_) {
        if (strcmp(k.keyId, keyId) == 0) return false;
    }

    ManagedKey key{};
    strncpy(key.keyId, keyId, kMaxKeyId - 1);
    key.purpose = purpose;
    key.state = KeyState::Active;
    memcpy(key.publicKey, publicKey, crypto::kKeySize);
    key.createdAt = static_cast<uint64_t>(std::time(nullptr));
    key.expiresAt = expiresAt;

    keys_.push_back(key);
    return true;
}

bool KeyManager::RevokeKey(const char* keyId, uint64_t nowTimestamp) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& k : keys_) {
        if (strcmp(k.keyId, keyId) == 0 && k.state == KeyState::Active) {
            k.state = KeyState::Revoked;
            k.revokedAt = nowTimestamp;
            return true;
        }
    }
    return false;
}

RotationResult KeyManager::RotateKey(const char* oldKeyId, uint64_t nowTimestamp) {
    RotationResult result{};

    std::lock_guard<std::mutex> lock(mutex_);

    ManagedKey* oldKey = nullptr;
    for (auto& k : keys_) {
        if (strcmp(k.keyId, oldKeyId) == 0) {
            oldKey = &k;
            break;
        }
    }

    if (!oldKey || oldKey->state != KeyState::Active) {
        strncpy(result.error, "old key not found or not active", sizeof(result.error) - 1);
        return result;
    }

    // Generate new key
    crypto::KeyPair newKp{};
    if (!crypto_.GenerateKeyPair(newKp)) {
        strncpy(result.error, "key generation failed", sizeof(result.error) - 1);
        return result;
    }

    // Build new key ID
    char newId[kMaxKeyId]{};
    snprintf(newId, kMaxKeyId, "%s-r%llu", oldKeyId,
             static_cast<unsigned long long>(nowTimestamp));

    ManagedKey newKey{};
    strncpy(newKey.keyId, newId, kMaxKeyId - 1);
    newKey.purpose = oldKey->purpose;
    newKey.state = KeyState::Active;
    memcpy(newKey.publicKey, newKp.publicKey, crypto::kKeySize);
    newKey.createdAt = nowTimestamp;
    newKey.expiresAt = nowTimestamp + (oldKey->expiresAt - oldKey->createdAt);

    // Mark old as rotating (still valid for verification, not for new signing)
    oldKey->state = KeyState::Rotating;

    keys_.push_back(newKey);

    strncpy(result.oldKeyId, oldKeyId, kMaxKeyId - 1);
    strncpy(result.newKeyId, newId, kMaxKeyId - 1);
    result.success = true;
    return result;
}

const ManagedKey* KeyManager::GetActiveKey(KeyPurpose purpose) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& k : keys_) {
        if (k.purpose == purpose && k.state == KeyState::Active)
            return &k;
    }
    return nullptr;
}

const ManagedKey* KeyManager::FindKey(const char* keyId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& k : keys_) {
        if (strcmp(k.keyId, keyId) == 0)
            return &k;
    }
    return nullptr;
}

bool KeyManager::IsKeyValid(const char* keyId, uint64_t nowTimestamp) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& k : keys_) {
        if (strcmp(k.keyId, keyId) == 0) {
            if (k.state == KeyState::Revoked || k.state == KeyState::Expired)
                return false;
            if (k.expiresAt > 0 && nowTimestamp > k.expiresAt)
                return false;
            return true;
        }
    }
    return false;
}

void KeyManager::ExpireOldKeys(uint64_t nowTimestamp) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& k : keys_) {
        if (k.state == KeyState::Active || k.state == KeyState::Rotating) {
            if (k.expiresAt > 0 && nowTimestamp > k.expiresAt) {
                k.state = KeyState::Expired;
            }
        }
    }
}

void KeyManager::RevokeAll(uint64_t nowTimestamp) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& k : keys_) {
        if (k.state == KeyState::Active || k.state == KeyState::Rotating) {
            k.state = KeyState::Revoked;
            k.revokedAt = nowTimestamp;
        }
    }
    issuanceSuspended_ = true;
}

} // namespace skr::ops
