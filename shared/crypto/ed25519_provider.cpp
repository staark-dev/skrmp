#include "crypto/crypto.h"
#include <sodium.h>
#include <cstring>

namespace skr::crypto {

static bool EnsureSodiumInit() {
    static bool initialized = [] {
        return sodium_init() >= 0; // returns 0 on first init, 1 if already done
    }();
    return initialized;
}

bool Ed25519Provider::GenerateKeyPair(KeyPair& out) {
    if (!EnsureSodiumInit()) return false;

    // libsodium generates a 64-byte "secret key" (seed + public key concatenated)
    // and a 32-byte public key. We store only the 32-byte seed as our private key.
    uint8_t pk[crypto_sign_ed25519_PUBLICKEYBYTES];
    uint8_t sk[crypto_sign_ed25519_SECRETKEYBYTES];

    if (crypto_sign_ed25519_keypair(pk, sk) != 0) return false;

    static_assert(kKeySize == crypto_sign_ed25519_SEEDBYTES, "Key size mismatch");
    static_assert(kSignatureSize == crypto_sign_ed25519_BYTES, "Signature size mismatch");

    memcpy(out.publicKey, pk, kKeySize);
    // Extract seed (first 32 bytes of libsodium's 64-byte secret key)
    memcpy(out.privateKey, sk, kKeySize);
    sodium_memzero(sk, sizeof(sk));

    return true;
}

bool Ed25519Provider::Sign(const uint8_t privateKey[kKeySize],
                           const uint8_t* message, size_t messageLen,
                           uint8_t signature[kSignatureSize]) {
    if (!EnsureSodiumInit()) return false;
    if (!message || messageLen == 0) return false;

    // Reconstruct the 64-byte secret key from seed.
    // libsodium needs seed → full secret key for signing.
    uint8_t pk[crypto_sign_ed25519_PUBLICKEYBYTES];
    uint8_t sk[crypto_sign_ed25519_SECRETKEYBYTES];

    if (crypto_sign_ed25519_seed_keypair(pk, sk, privateKey) != 0) {
        sodium_memzero(sk, sizeof(sk));
        return false;
    }

    unsigned long long sigLen = 0;
    int rc = crypto_sign_ed25519_detached(signature, &sigLen, message, messageLen, sk);
    sodium_memzero(sk, sizeof(sk));

    return rc == 0 && sigLen == kSignatureSize;
}

bool Ed25519Provider::Verify(const uint8_t publicKey[kKeySize],
                             const uint8_t* message, size_t messageLen,
                             const uint8_t signature[kSignatureSize]) {
    if (!EnsureSodiumInit()) return false;
    if (!message || messageLen == 0) return false;

    return crypto_sign_ed25519_verify_detached(signature, message, messageLen, publicKey) == 0;
}

bool Ed25519Provider::RandomBytes(uint8_t* out, size_t len) {
    if (!EnsureSodiumInit()) return false;
    randombytes_buf(out, len);
    return true;
}

} // namespace skr::crypto
