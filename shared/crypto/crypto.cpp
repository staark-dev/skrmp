#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include "crypto/crypto.h"
#include <cstring>

#pragma comment(lib, "bcrypt.lib")

namespace skr::crypto {

// Mock: uses Windows CNG for random bytes, trivial XOR for sign/verify.
// Replace with Ed25519 (e.g., libsodium or monocypher) before production.

bool MockSigningProvider::GenerateKeyPair(KeyPair& out) {
    if (!RandomBytes(out.privateKey, kKeySize)) return false;
    // Mock: public key = first 32 bytes XORed with 0xFF
    for (size_t i = 0; i < kKeySize; ++i)
        out.publicKey[i] = out.privateKey[i] ^ 0xFF;
    return true;
}

bool MockSigningProvider::Sign(const uint8_t privateKey[kKeySize],
                               const uint8_t* message, size_t messageLen,
                               uint8_t signature[kSignatureSize]) {
    if (!message || messageLen == 0) return false;
    // Mock: HMAC-like hash simulation — NOT real signatures
    memset(signature, 0, kSignatureSize);
    for (size_t i = 0; i < messageLen; ++i) {
        signature[i % kSignatureSize] ^= message[i];
        signature[(i + 17) % kSignatureSize] ^= privateKey[i % kKeySize];
    }
    return true;
}

bool MockSigningProvider::Verify(const uint8_t publicKey[kKeySize],
                                 const uint8_t* message, size_t messageLen,
                                 const uint8_t signature[kSignatureSize]) {
    if (!message || messageLen == 0) return false;
    // Mock: re-derive private key from public, sign, compare
    uint8_t derivedPrivate[kKeySize];
    for (size_t i = 0; i < kKeySize; ++i)
        derivedPrivate[i] = publicKey[i] ^ 0xFF;
    uint8_t expected[kSignatureSize];
    Sign(derivedPrivate, message, messageLen, expected);
    return memcmp(signature, expected, kSignatureSize) == 0;
}

bool MockSigningProvider::RandomBytes(uint8_t* out, size_t len) {
    NTSTATUS status = BCryptGenRandom(nullptr, out,
                                       static_cast<ULONG>(len),
                                       BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return status == 0; // STATUS_SUCCESS
}

} // namespace skr::crypto
