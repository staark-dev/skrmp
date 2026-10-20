#pragma once
#include <cstdint>

namespace skr::protocol {
// SKR-owned envelope multiplexed on 240 (client -> server), 241 (server -> client).
// Existing v1 data payloads remain unchanged. This is capability negotiation,
// never identity, launcher authentication, or authorization.
constexpr uint8_t kEnhancedMarker = 0xE0;
constexpr uint8_t kEnhancedVersion = 1;
constexpr uint8_t kEnhancedCapabilities = 0x07; // session info, client state, radar
constexpr uint64_t kEnhancedTimeoutMs = 10000;
enum class ClientMode : uint8_t { Standard, EnhancedCandidate, Enhanced };
enum class EnhancedStep : uint8_t { Hello = 1, Offer = 2, Confirm = 3 };
struct EnhancedSession {
    ClientMode mode = ClientMode::Standard;
    uint32_t token = 0;
    uint64_t deadline = 0;
    bool Begin(uint8_t version, uint8_t capabilities, uint32_t generation, uint64_t now) {
        if (mode != ClientMode::Standard || version != kEnhancedVersion ||
            capabilities != kEnhancedCapabilities || !generation) return false;
        mode = ClientMode::EnhancedCandidate; token = generation;
        deadline = now + kEnhancedTimeoutMs;
        return true;
    }
    bool Confirm(uint8_t version, uint8_t capabilities, uint32_t echoed, uint64_t now) {
        if (mode != ClientMode::EnhancedCandidate || now >= deadline ||
            version != kEnhancedVersion || capabilities != kEnhancedCapabilities ||
            echoed != token) return false;
        mode = ClientMode::Enhanced; deadline = 0; return true;
    }
    void Expire(uint64_t now) {
        if (mode == ClientMode::EnhancedCandidate && now >= deadline) *this = {};
    }
};
}
