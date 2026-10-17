#pragma once
#include <cstdint>
#include <cstring>

namespace skr::protocol {

constexpr uint16_t kProtocolVersion = 1;
constexpr size_t kNonceSize = 32;
constexpr size_t kJoinIdSize = 32;       // opaque 256-bit random
constexpr size_t kPublicKeySize = 32;    // Ed25519 public key
constexpr size_t kSignatureSize = 64;    // Ed25519 signature
constexpr size_t kConnectionIdSize = 16;
constexpr size_t kMaxServerIdLen = 64;
constexpr size_t kMaxHandshakeMsg = 2048;
constexpr uint32_t kTicketLifetimeSec = 30;
constexpr uint32_t kChallengeTimeoutSec = 10;
constexpr uint32_t kVerifyTimeoutSec = 5;

enum class GateState : uint8_t {
    Connected,
    Quarantined,
    Challenged,
    Verifying,
    Authorized,
    Rejected
};

enum class AuthError : uint8_t {
    None = 0,
    AuthRequired,
    TicketExpired,
    TicketUsed,
    BuildUnsupported,
    ProtocolUnsupported,
    AcRequired,
    ServiceUnavailable,
    InvalidSignature,
    ConnectionMismatch,
    InternalError
};

// Wire format uses network byte order, fixed sizes, length-prefixed strings.
// All multi-byte integers are big-endian on the wire.

#pragma pack(push, 1)

struct ChallengeMessage {
    uint16_t version;
    char server_id[kMaxServerIdLen];
    uint8_t server_id_len;
    uint8_t connection_id[kConnectionIdSize];
    uint64_t generation;
    uint8_t nonce[kNonceSize];
    uint64_t expires_at;   // UNIX seconds
};

struct ProofMessage {
    uint16_t version;
    uint8_t join_id[kJoinIdSize];
    uint8_t signature[kSignatureSize];
};

struct ResultMessage {
    uint16_t version;
    uint8_t status;       // 1 = authorized, 0 = rejected
    uint8_t error_code;   // AuthError
};

#pragma pack(pop)

// Canonical transcript for signing (matches master plan section 9.3)
// "SKR-JOIN-v1" || protocol_u16_be ||
// len16(server_id) || server_id_utf8 ||
// connection_id_16bytes || generation_u64_be || nonce_32bytes ||
// len16(join_id) || join_id_ascii || expires_at_u64_be
struct TranscriptBuilder {
    uint8_t buffer[512];
    size_t length = 0;

    void Reset() { length = 0; }

    bool Build(const ChallengeMessage& challenge, const uint8_t joinId[kJoinIdSize]) {
        Reset();
        // Prefix
        const char prefix[] = "SKR-JOIN-v1";
        Append(reinterpret_cast<const uint8_t*>(prefix), sizeof(prefix) - 1);
        // Protocol version (big-endian)
        AppendU16(challenge.version);
        // Server ID (length-prefixed)
        AppendU16(challenge.server_id_len);
        Append(reinterpret_cast<const uint8_t*>(challenge.server_id), challenge.server_id_len);
        // Connection ID
        Append(challenge.connection_id, kConnectionIdSize);
        // Generation (big-endian)
        AppendU64(challenge.generation);
        // Nonce
        Append(challenge.nonce, kNonceSize);
        // Join ID (length-prefixed)
        AppendU16(static_cast<uint16_t>(kJoinIdSize));
        Append(joinId, kJoinIdSize);
        // Expires at (big-endian)
        AppendU64(challenge.expires_at);
        return length <= sizeof(buffer);
    }

private:
    void Append(const uint8_t* data, size_t len) {
        if (length + len <= sizeof(buffer)) {
            memcpy(buffer + length, data, len);
            length += len;
        }
    }
    void AppendU16(uint16_t val) {
        uint8_t b[2] = { static_cast<uint8_t>(val >> 8), static_cast<uint8_t>(val) };
        Append(b, 2);
    }
    void AppendU64(uint64_t val) {
        uint8_t b[8];
        for (int i = 7; i >= 0; --i) { b[i] = static_cast<uint8_t>(val); val >>= 8; }
        Append(b, 8);
    }
};

} // namespace skr::protocol
