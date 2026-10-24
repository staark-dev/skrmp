#pragma once
#include <cstdint>
#include <cstring>

#include "protocol/enhanced.h"
#include "protocol/native_api.h"

// SKR-X Extension Gateway (M1).
//
// Multiplexed on RPC 240 (C->S) and 241 (S->C) using envelope step 10.
// Inner routing by namespace byte. Each namespace has its own version.
//
// Wire format:
//   u8  marker      = 0xE0
//   u8  step        = 10
//   u8  namespace
//   u8  version     = namespace protocol version
//   u16 payloadLen  (little-endian)
//   ... payload

namespace skr::protocol::skrx {

constexpr uint8_t kGatewayStep = 10;

enum class Namespace : uint8_t {
    Core        = 0,
    Vehicles    = 1,
    Assets      = 2,
    Hud         = 3,
    Gps         = 4,
    Phone       = 5,   // PHONE v1, requires CapPhoneGateway + CapPhone + CapGateway
    Ui          = 6,
    Diagnostics = 7,
};
constexpr uint8_t kNamespaceCount = 8;
constexpr bool IsKnownNamespace(uint8_t ns) { return ns < kNamespaceCount; }

// ── CORE namespace (M1) ─────────────────────────────────────────────
constexpr uint8_t kCoreVersion = 1;

enum class CoreOp : uint8_t {
    Ping = 1,   // C->S: u32 clientTimestamp
    Pong = 2,   // S->C: u32 echoedTimestamp, u32 serverTimestamp
};

// ── Gateway header ──────────────────────────────────────────────────
constexpr size_t kGatewayHeaderSize = 6;   // marker + step + ns + version + payloadLen(2)
constexpr size_t kMaxGatewayPayload = 400;
constexpr size_t kMaxGatewaySize    = kGatewayHeaderSize + kMaxGatewayPayload;

struct GatewayHeader {
    Namespace ns = Namespace::Core;
    uint8_t   version = 0;
    uint16_t  payloadLen = 0;
};

inline size_t EncodeGateway(const GatewayHeader& hdr, const uint8_t* payload,
                            uint8_t* out, size_t cap) {
    const size_t total = kGatewayHeaderSize + hdr.payloadLen;
    if (total > cap || hdr.payloadLen > kMaxGatewayPayload) return 0;
    out[0] = kEnhancedMarker;
    out[1] = kGatewayStep;
    out[2] = static_cast<uint8_t>(hdr.ns);
    out[3] = hdr.version;
    out[4] = static_cast<uint8_t>(hdr.payloadLen);
    out[5] = static_cast<uint8_t>(hdr.payloadLen >> 8);
    if (hdr.payloadLen && payload) memcpy(out + kGatewayHeaderSize, payload, hdr.payloadLen);
    return total;
}

inline bool DecodeGateway(const uint8_t* data, size_t len, GatewayHeader& hdr,
                          const uint8_t*& payload) {
    if (!data || len < kGatewayHeaderSize) return false;
    if (data[0] != kEnhancedMarker || data[1] != kGatewayStep) return false;
    hdr.ns = static_cast<Namespace>(data[2]);
    hdr.version = data[3];
    hdr.payloadLen = uint16_t(data[4]) | (uint16_t(data[5]) << 8);
    if (hdr.payloadLen > kMaxGatewayPayload) return false;
    if (len != kGatewayHeaderSize + hdr.payloadLen) return false;
    payload = (hdr.payloadLen > 0) ? data + kGatewayHeaderSize : nullptr;
    return true;
}

// ── CORE Ping (C->S, 4 bytes payload) ──────────────────────────────
struct CorePing {
    uint32_t clientTimestamp = 0;
};

inline size_t EncodePing(const CorePing& m, uint8_t* out, size_t cap) {
    if (cap < 4) return 0;
    out[0] = uint8_t(m.clientTimestamp);
    out[1] = uint8_t(m.clientTimestamp >> 8);
    out[2] = uint8_t(m.clientTimestamp >> 16);
    out[3] = uint8_t(m.clientTimestamp >> 24);
    return 4;
}

inline bool DecodePing(const uint8_t* p, size_t n, CorePing& m) {
    if (n != 4) return false;
    m.clientTimestamp = uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
                        (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
    return true;
}

// ── CORE Pong (S->C, 8 bytes payload) ──────────────────────────────
struct CorePong {
    uint32_t echoedTimestamp = 0;
    uint32_t serverTimestamp = 0;
};

inline size_t EncodePong(const CorePong& m, uint8_t* out, size_t cap) {
    if (cap < 8) return 0;
    api::Writer w(out, cap);
    w.U32(m.echoedTimestamp);
    w.U32(m.serverTimestamp);
    return w.size();
}

inline bool DecodePong(const uint8_t* p, size_t n, CorePong& m) {
    if (n != 8) return false;
    api::Reader r(p, n);
    m.echoedTimestamp = r.U32();
    m.serverTimestamp = r.U32();
    return r.Done();
}

// ── Full gateway message encode helpers ─────────────────────────────
// Encode a CORE:Ping as a complete gateway message (header + op byte + payload).
inline size_t EncodeGatewayPing(const CorePing& ping, uint8_t* out, size_t cap) {
    uint8_t inner[8];
    inner[0] = static_cast<uint8_t>(CoreOp::Ping);
    size_t pLen = EncodePing(ping, inner + 1, sizeof(inner) - 1);
    if (!pLen) return 0;
    GatewayHeader hdr;
    hdr.ns = Namespace::Core;
    hdr.version = kCoreVersion;
    hdr.payloadLen = static_cast<uint16_t>(1 + pLen);
    return EncodeGateway(hdr, inner, out, cap);
}

inline size_t EncodeGatewayPong(const CorePong& pong, uint8_t* out, size_t cap) {
    uint8_t inner[16];
    inner[0] = static_cast<uint8_t>(CoreOp::Pong);
    size_t pLen = EncodePong(pong, inner + 1, sizeof(inner) - 1);
    if (!pLen) return 0;
    GatewayHeader hdr;
    hdr.ns = Namespace::Core;
    hdr.version = kCoreVersion;
    hdr.payloadLen = static_cast<uint16_t>(1 + pLen);
    return EncodeGateway(hdr, inner, out, cap);
}

// Check if raw data starts with the gateway envelope (step 10).
inline bool IsGatewayMessage(const uint8_t* data, size_t len) {
    return data && len >= 2 && data[0] == kEnhancedMarker && data[1] == kGatewayStep;
}

} // namespace skr::protocol::skrx
