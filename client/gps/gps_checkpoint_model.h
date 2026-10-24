#pragma once
// ═══════════════════════════════════════════════════════════════════════
// GPS world checkpoint — pure state (API-003.3)
// ═══════════════════════════════════════════════════════════════════════
// Which 3D destination markers exist, where, how they look, and when they go
// away. No GTA memory here (unit-tested in test_gps_checkpoint.cpp); the GTA
// side (gps_checkpoint.cpp) supplies ground heights and places the markers
// through C3dMarkers every frame.
//
// Slot 0 follows the local GPS waypoint at its EXACT coordinates (never the
// road-snapped route end). Slots 1..kMaxServer are server destinations
// (future Pawn natives, API-003.3 draft). Client-side arrival is advisory:
// it only drives the visual policy and an event queue the Gate may report;
// mission completion must be validated by the server.

#include "gps_types.h"

#include <cmath>
#include <cstdint>

namespace skr::gps::checkpoint {

constexpr int kMaxServer = 4;
constexpr int kSlots     = 1 + kMaxServer;   // [0] local waypoint, [1..] server ids 1..kMaxServer

enum class ArrivalPolicy : uint8_t {
    Remove,            // hide on arrival
    KeepFor,           // keep keepMs after arrival, then hide
    KeepUntilCleared,  // keep until replaced / removed / waypoint cleared by the player
};

struct Style {
    uint8_t  r = 163, g = 230, b = 91, a = 140;   // SKR lime, translucent
    float    size = 4.0f;            // GTA cylinder size (world units)
    float    drawDistance = 250.0f;  // world units from the player
    uint16_t pulsePeriod = 1024;     // ms, 0 = steady
    float    pulseFraction = 0.12f;
    bool     visible = true;
};

struct Marker {
    bool          active = false;
    Vec3          pos = {};
    bool          hasZ = false;        // z given by the owner (server); else ground lookup
    bool          groundKnown = false;
    float         groundZ = 0.0f;
    Style         style;
    ArrivalPolicy policy = ArrivalPolicy::Remove;
    uint32_t      keepMs = 0;
    float         arrivalRadius = 0.0f;   // server: advisory entry radius (0 = style.size)
    bool          arrived = false;
    uint32_t      arrivedAt = 0;
    bool          hidden = false;         // arrived + policy expired, kept for de-dup
};

struct Placement {
    int   slot;
    Vec3  pos;      // z resolved
    Style style;
};

inline float Dist2D(const Vec3& a, const Vec3& b) { return std::hypot(a.x - b.x, a.y - b.y); }

class Controller {
public:
    void Reset() { *this = Controller(); }

    // ── Local waypoint (slot 0) ─────────────────────────────────────────
    void SetLocalStyle(const Style& s) { m_localStyle = s; m_slots[0].style = s; }
    void SetLocalPolicy(ArrivalPolicy p, uint32_t keepMs) {
        m_localPolicy = p; m_localKeepMs = keepMs;
        m_slots[0].policy = p; m_slots[0].keepMs = keepMs;
    }
    const Style& LocalStyle() const { return m_localStyle; }

    // Called on the game tick with the engine destination (= the waypoint
    // the player set, exact coordinates). Arrival/cancel arrive as events
    // first (OnLocalArrived / OnLocalCancelled) within the same tick.
    void SyncLocal(bool hasWaypoint, const Vec3& waypoint, uint32_t now) {
        Marker& m = m_slots[0];
        if (hasWaypoint) {
            if (!m.active || m.arrived || Dist2D(m.pos, waypoint) > 1.0f) {
                m = Marker();
                m.active = true;
                m.pos = waypoint;
                m.style = m_localStyle;
                m.policy = m_localPolicy;
                m.keepMs = m_localKeepMs;
            }
            return;
        }
        if (!m.active) return;
        if (!m.arrived) { m = Marker(); return; }    // waypoint gone without arrival = cleared
        ApplyPolicy(m, now);
    }
    void OnLocalArrived(uint32_t now) {
        Marker& m = m_slots[0];
        if (!m.active || m.arrived) return;
        m.arrived = true;
        m.arrivedAt = now;
        ApplyPolicy(m, now);
    }
    void OnLocalCancelled() { m_slots[0] = Marker(); }   // always removed, whatever the policy
    void ClearLocal() { m_slots[0] = Marker(); }         // explicit clear (KeepUntilCleared)

    // ── Server destinations (ids 1..kMaxServer) ──────────────────────────
    // Create or modify. Returns false for an invalid id or position.
    bool SetServer(int id, const Vec3& pos, bool hasZ, const Style& style, ArrivalPolicy policy,
                   uint32_t keepMs, float arrivalRadius) {
        if (id < 1 || id > kMaxServer) return false;
        if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z)) return false;
        if (std::fabs(pos.x) > 3000.0f || std::fabs(pos.y) > 3000.0f) return false;
        if (!(style.size > 0.0f && style.size <= 50.0f && style.drawDistance > 0.0f)) return false;
        Marker& m = m_slots[id];
        const bool moved = !m.active || Dist2D(m.pos, pos) > 0.01f || m.hasZ != hasZ ||
                           (hasZ && std::fabs(m.pos.z - pos.z) > 0.01f);
        if (moved) { m = Marker(); m.pos = pos; m.hasZ = hasZ; }
        m.active = true;
        m.style = style;
        m.policy = policy;
        m.keepMs = keepMs;
        m.arrivalRadius = arrivalRadius;
        return true;
    }
    bool RemoveServer(int id) {
        if (id < 1 || id > kMaxServer || !m_slots[id].active) return false;
        m_slots[id] = Marker();
        return true;
    }
    void ClearServer() { for (int i = 1; i < kSlots; ++i) m_slots[i] = Marker(); }

    // Advisory entry detection for server markers (once per marker).
    void UpdateServerArrival(const Vec3& player, uint32_t now) {
        for (int i = 1; i < kSlots; ++i) {
            Marker& m = m_slots[i];
            if (!m.active || m.arrived) continue;
            const float radius = m.arrivalRadius > 0.0f ? m.arrivalRadius : m.style.size;
            const float d = Dist2D(m.pos, player);
            const float z = m.hasZ ? m.pos.z : (m.groundKnown ? m.groundZ : player.z);
            if (std::fabs(z - player.z) > 4.0f + radius) continue;   // different level
            if (d > radius) continue;
            m.arrived = true;
            m.arrivedAt = now;
            PushArrival(i);
            ApplyPolicy(m, now);
        }
    }
    bool TakeArrival(int& id) {
        if (m_arrivalCount == 0) return false;
        id = m_arrivals[0];
        for (int i = 1; i < m_arrivalCount; ++i) m_arrivals[i - 1] = m_arrivals[i];
        --m_arrivalCount;
        return true;
    }

    // ── Ground height (owner gave no z) ─────────────────────────────────
    // Slots whose ground height is still needed and that are close enough
    // for GTA to have their collision streamed (draw distance + margin).
    bool NeedsGround(int slot, const Vec3& player) const {
        const Marker& m = m_slots[slot];
        return m.active && !m.hidden && !m.hasZ && !m.groundKnown &&
               Dist2D(m.pos, player) <= m.style.drawDistance + 50.0f;
    }
    void SetGround(int slot, float z) {
        Marker& m = m_slots[slot];
        if (!m.active || !std::isfinite(z)) return;
        m.groundZ = z;
        m.groundKnown = true;
    }

    // ── Per frame: what to place this frame ──────────────────────────────
    int Collect(const Vec3& player, uint32_t now, Placement* out, int max) {
        int n = 0;
        for (int i = 0; i < kSlots && n < max; ++i) {
            Marker& m = m_slots[i];
            if (m.arrived) ApplyPolicy(m, now);
            if (!m.active || m.hidden || !m.style.visible) continue;
            if (!m.hasZ && !m.groundKnown) continue;               // no fake height
            if (Dist2D(m.pos, player) > m.style.drawDistance) continue;
            if (i == 0 && DuplicatesServer(m)) continue;           // one marker per place
            Vec3 p = m.pos;
            if (!m.hasZ) p.z = m.groundZ;
            out[n++] = {i, p, m.style};
        }
        return n;
    }

    const Marker& Slot(int i) const { return m_slots[i]; }

private:
    void ApplyPolicy(Marker& m, uint32_t now) {
        switch (m.policy) {
            case ArrivalPolicy::Remove:           m.hidden = true; break;
            case ArrivalPolicy::KeepFor:          m.hidden = now - m.arrivedAt >= m.keepMs; break;
            case ArrivalPolicy::KeepUntilCleared: break;
        }
        if (m.hidden && &m == &m_slots[0]) m_slots[0] = Marker();   // local: gone for good
    }
    bool DuplicatesServer(const Marker& local) const {
        for (int i = 1; i < kSlots; ++i)
            if (m_slots[i].active && !m_slots[i].hidden && Dist2D(m_slots[i].pos, local.pos) < 3.0f) return true;
        return false;
    }
    void PushArrival(int id) {
        if (m_arrivalCount == 8) { for (int i = 1; i < 8; ++i) m_arrivals[i - 1] = m_arrivals[i]; --m_arrivalCount; }
        m_arrivals[m_arrivalCount++] = id;
    }

    Marker        m_slots[kSlots];
    Style         m_localStyle;
    ArrivalPolicy m_localPolicy = ArrivalPolicy::Remove;
    uint32_t      m_localKeepMs = 0;
    int           m_arrivals[8] = {};
    int           m_arrivalCount = 0;
};

} // namespace skr::gps::checkpoint
