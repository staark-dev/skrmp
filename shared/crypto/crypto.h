#pragma once
#include <cstdint>
#include <cstddef>

namespace skr::crypto {

constexpr size_t kKeySize = 32;
constexpr size_t kSignatureSize = 64;

struct KeyPair {
    uint8_t publicKey[kKeySize];
    uint8_t privateKey[kKeySize];  // seed form (32 bytes)
};

class ISigningProvider {
public:
    virtual ~ISigningProvider() = default;
    virtual bool GenerateKeyPair(KeyPair& out) = 0;
    virtual bool Sign(const uint8_t privateKey[kKeySize],
                      const uint8_t* message, size_t messageLen,
                      uint8_t signature[kSignatureSize]) = 0;
    virtual bool Verify(const uint8_t publicKey[kKeySize],
                        const uint8_t* message, size_t messageLen,
                        const uint8_t signature[kSignatureSize]) = 0;
    virtual bool RandomBytes(uint8_t* out, size_t len) = 0;
};

// Mock provider for development — deterministic, NOT cryptographically secure.
class MockSigningProvider final : public ISigningProvider {
public:
    bool GenerateKeyPair(KeyPair& out) override;
    bool Sign(const uint8_t privateKey[kKeySize],
              const uint8_t* message, size_t messageLen,
              uint8_t signature[kSignatureSize]) override;
    bool Verify(const uint8_t publicKey[kKeySize],
                const uint8_t* message, size_t messageLen,
                const uint8_t signature[kSignatureSize]) override;
    bool RandomBytes(uint8_t* out, size_t len) override;
};

// Production Ed25519 provider backed by libsodium.
class Ed25519Provider final : public ISigningProvider {
public:
    bool GenerateKeyPair(KeyPair& out) override;
    bool Sign(const uint8_t privateKey[kKeySize],
              const uint8_t* message, size_t messageLen,
              uint8_t signature[kSignatureSize]) override;
    bool Verify(const uint8_t publicKey[kKeySize],
                const uint8_t* message, size_t messageLen,
                const uint8_t signature[kSignatureSize]) override;
    bool RandomBytes(uint8_t* out, size_t len) override;
};

} // namespace skr::crypto
