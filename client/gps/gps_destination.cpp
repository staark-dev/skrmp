#include "gps_destination.h"
#include <cstring>
#include <windows.h>

namespace skr::gps {

// ── GTA addresses (SA 1.0 US) ──────────────────────────────────────
// CRadar::ms_RadarTrace — array of 175 tRadarTrace structs (0x28 each)
static constexpr uintptr_t kRadarTraceBase = 0xBA86F0;
static constexpr int       kMaxRadarTraces = 175;

// tRadarTrace layout (from plugin-sdk CRadar.h)
static constexpr int kTraceSize          = 0x28;
static constexpr int kTraceOffPos        = 0x08;   // CVector m_vecPos
static constexpr int kTraceOffSprite     = 0x24;   // unsigned char m_nRadarSprite
static constexpr int kTraceOffFlags      = 0x25;   // bitfield byte: bit1 = m_bInUse
static constexpr int kTraceOffTypeDisp   = 0x26;   // bitfield: bits 2..5 = m_nBlipType
static constexpr int kRadarSpriteWaypoint = 41;

// ── Module state ────────────────────────────────────────────────────
static bool  g_hasDestination  = false;
static Vec3  g_destination     = {};
static float g_arrivalRadius   = 15.0f;

// ── Map waypoint detection ──────────────────────────────────────────

bool HasMapWaypoint() {
    __try {
        auto* base = reinterpret_cast<const uint8_t*>(kRadarTraceBase);
        for (int i = 0; i < kMaxRadarTraces; ++i) {
            const uint8_t* trace = base + i * kTraceSize;
            uint8_t sprite = trace[kTraceOffSprite];
            uint8_t flags  = trace[kTraceOffFlags];
            bool inUse     = (flags & 0x02) != 0;
            if (inUse && sprite == kRadarSpriteWaypoint)
                return true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

Vec3 GetMapWaypointPosition() {
    Vec3 result{};
    __try {
        auto* base = reinterpret_cast<const uint8_t*>(kRadarTraceBase);
        for (int i = 0; i < kMaxRadarTraces; ++i) {
            const uint8_t* trace = base + i * kTraceSize;
            uint8_t sprite = trace[kTraceOffSprite];
            uint8_t flags  = trace[kTraceOffFlags];
            bool inUse     = (flags & 0x02) != 0;
            if (inUse && sprite == kRadarSpriteWaypoint) {
                auto* pos = reinterpret_cast<const float*>(trace + kTraceOffPos);
                result.x = pos[0];
                result.y = pos[1];
                result.z = pos[2];
                return result;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return result;
}

// ── Native waypoint removal ─────────────────────────────────────────
// plugin-sdk CRadar.cpp / CMenuManager.h (verified API-003.0.1).
static constexpr uintptr_t kClearBlip        = 0x587CE0;          // void __cdecl CRadar::ClearBlip(int)
static constexpr uintptr_t kTargetBlipIndex  = 0xBA6748 + 0x2C;   // CMenuManager::m_nTargetBlipIndex
static constexpr int       kTraceOffCounter  = 0x14;              // m_nCounter (handle high word)
using ClearBlipFn = void(__cdecl*)(int handle);

static int FindWaypointTrace() {
    __try {
        auto* base = reinterpret_cast<const uint8_t*>(kRadarTraceBase);
        for (int i = 0; i < kMaxRadarTraces; ++i) {
            const uint8_t* trace = base + i * kTraceSize;
            if ((trace[kTraceOffFlags] & 0x02) && trace[kTraceOffSprite] == kRadarSpriteWaypoint)
                return i;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return -1;
}

bool ClearMapWaypoint() {
    __try {
        auto clearBlip = reinterpret_cast<ClearBlipFn>(kClearBlip);
        auto* target = reinterpret_cast<int*>(kTargetBlipIndex);
        if (*target != 0) {
            clearBlip(*target);
            *target = 0;
        }
        // Handle = arrayIndex | (m_nCounter << 16) (plugin-sdk tBlipHandle).
        for (int guard = 0; guard < kMaxRadarTraces; ++guard) {
            const int i = FindWaypointTrace();
            if (i < 0) return true;
            const auto* trace = reinterpret_cast<const uint8_t*>(kRadarTraceBase + i * kTraceSize);
            const uint16_t counter = *reinterpret_cast<const uint16_t*>(trace + kTraceOffCounter);
            clearBlip(static_cast<int>(static_cast<uint32_t>(i) | (static_cast<uint32_t>(counter) << 16)));
            if (FindWaypointTrace() == i) return false;   // ClearBlip refused: never loop
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return FindWaypointTrace() < 0;
}

// ── Destination management ──────────────────────────────────────────

void SetDestination(const Vec3& pos) {
    if (g_hasDestination &&
        std::abs(g_destination.x - pos.x) < 1.0f &&
        std::abs(g_destination.y - pos.y) < 1.0f &&
        std::abs(g_destination.z - pos.z) < 1.0f)
        return;

    g_destination    = pos;
    g_hasDestination = true;
}

void ClearDestination() {
    g_hasDestination = false;
    g_destination    = {};
}

bool HasDestination() {
    return g_hasDestination;
}

Vec3 GetDestination() {
    return g_destination;
}

void SetArrivalRadius(float radius) {
    if (radius > 0.0f) g_arrivalRadius = radius;
}

float GetArrivalRadius() {
    return g_arrivalRadius;
}

bool IsArrived(const Vec3& playerPos) {
    if (!g_hasDestination) return false;
    return playerPos.DistanceTo2D(g_destination) <= g_arrivalRadius;
}

} // namespace skr::gps
