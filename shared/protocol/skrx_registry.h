#pragma once
#include <cstdint>
#include <cstring>

// SKR-X Unified RPC Registry (M1).
//
// Central registry for all SKR-owned RPC handlers. Keyed on (rpcId, step, direction).
// Populated at startup; used for dispatch validation and collision detection.
// Does NOT replace existing open.mp or RakClient handler registration — it wraps
// them with direction checks and feature gating.

namespace skr::protocol::skrx {

enum class Direction : uint8_t { Inbound = 0, Outbound = 1 };

using HandlerFn = void(*)(void* ctx, int playerId, const uint8_t* data, size_t len);

struct Entry {
    uint8_t     rpcId;
    uint8_t     envelopeStep;   // 0 = raw RPC (non-envelope), 1+ = envelope step
    Direction   direction;
    const char* name;           // short slug, e.g. "client-state"
    uint16_t    featureFlags;   // bitmask: required API capabilities (0 = none)
    bool        requireEnhanced;
    bool        requireApi;
    HandlerFn   handler;        // optional: nullptr for outbound-only entries
    void*       handlerCtx;
};

// Fixed-capacity registry. No heap allocation, no exceptions.
class Registry {
public:
    static constexpr int kMaxEntries = 64;

    bool Register(const Entry& entry) {
        if (count_ >= kMaxEntries) return false;
        if (Find(entry.rpcId, entry.envelopeStep, entry.direction))
            return false;
        entries_[count_++] = entry;
        return true;
    }

    bool Unregister(uint8_t rpcId, uint8_t step, Direction dir) {
        for (int i = 0; i < count_; ++i) {
            if (entries_[i].rpcId == rpcId && entries_[i].envelopeStep == step &&
                entries_[i].direction == dir) {
                entries_[i] = entries_[count_ - 1];
                --count_;
                return true;
            }
        }
        return false;
    }

    const Entry* Find(uint8_t rpcId, uint8_t step, Direction dir) const {
        for (int i = 0; i < count_; ++i) {
            if (entries_[i].rpcId == rpcId && entries_[i].envelopeStep == step &&
                entries_[i].direction == dir)
                return &entries_[i];
        }
        return nullptr;
    }

    int Count() const { return count_; }
    const Entry& At(int i) const { return entries_[i]; }
    void Clear() { count_ = 0; }

private:
    Entry entries_[kMaxEntries]{};
    int count_ = 0;
};

// Populate with all existing SKR RPC entries (steps 0–9 + dedicated IDs).
// Called once at startup. Returns false if a collision is detected.
bool RegisterBuiltinEntries(Registry& reg);

} // namespace skr::protocol::skrx
