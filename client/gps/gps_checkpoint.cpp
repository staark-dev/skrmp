#include "gps_checkpoint.h"
#include "gps_destination.h"
#include "gps_feedback.h"
#include "gps_flags.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace skr::gps::checkpoint {
namespace {

// GTA SA 1.0 US (plugin-sdk C3dMarkers.cpp / CWorld.cpp; prologues and call
// conventions checked in D:\SA-MP Reverse\gta_sa.bak.exe with dumpbin 14.51).
constexpr uintptr_t kPlaceMarker          = 0x725120;   // C3dMarkers::PlaceMarker, cdecl
constexpr uintptr_t kFindGroundZFor3DCoord = 0x5696C0;  // CWorld::FindGroundZFor3DCoord, cdecl
constexpr uint16_t  kMarkerCylinder       = 1;          // MARKER3D_CYLINDER (no eviction)

struct CVector { float x, y, z; };
using PlaceMarkerFn = void*(__cdecl*)(unsigned int, unsigned short, CVector&, float, unsigned char, unsigned char,
                                      unsigned char, unsigned char, unsigned short, float, short, float, float,
                                      float, bool);
using FindGroundFn = float(__cdecl*)(float, float, float, bool*, void**);

Controller g_ctl;
uint8_t    g_idTags[kSlots];          // their addresses are the GTA marker ids
Vec3       g_player = {};
bool       g_playerKnown = false;
int        g_subscription = 0;
uint32_t   g_lastGroundTry[kSlots] = {};
Stats      g_stats{};

void OnEvent(const feedback::Event& e, void*) {
    if (e.type == feedback::EventType::Arrived)   g_ctl.OnLocalArrived(GetTickCount());
    if (e.type == feedback::EventType::Cancelled) g_ctl.OnLocalCancelled();
}

bool GroundZ(float x, float y, float& z) {
    __try {
        bool found = false;
        z = reinterpret_cast<FindGroundFn>(kFindGroundZFor3DCoord)(x, y, 1000.0f, &found, nullptr);
        return found;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ++g_stats.groundFaults;
        return false;
    }
}

bool Place(unsigned int id, const Placement& p) {
    __try {
        CVector pos{p.pos.x, p.pos.y, p.pos.z};
        return reinterpret_cast<PlaceMarkerFn>(kPlaceMarker)(
            id, kMarkerCylinder, pos, p.style.size, p.style.r, p.style.g, p.style.b, p.style.a,
            p.style.pulsePeriod, p.style.pulseFraction, 0, 0.0f, 0.0f, 0.0f, false) != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

void Tick(float x, float y, float z) {
    if (!g_subscription) g_subscription = feedback::Subscribe(&OnEvent, nullptr);
    const uint32_t now = GetTickCount();
    g_player = {x, y, z};
    g_playerKnown = true;
    // Engine destination = the waypoint the player set, exact coordinates.
    g_ctl.SyncLocal(HasDestination(), GetDestination(), now);
    g_ctl.UpdateServerArrival(g_player, now);
    for (int i = 0; i < kSlots; ++i) {
        if (!g_ctl.NeedsGround(i, g_player) || now - g_lastGroundTry[i] < 500) continue;
        g_lastGroundTry[i] = now;
        ++g_stats.groundQueries;
        const Marker& m = g_ctl.Slot(i);
        float gz = 0.0f;
        if (GroundZ(m.pos.x, m.pos.y, gz)) g_ctl.SetGround(i, gz);
    }
}

void PlaceFrame() {
    if (!g_playerKnown || !flags::RendererEnabled()) return;
    Placement list[kSlots];
    const int n = g_ctl.Collect(g_player, GetTickCount(), list, kSlots);
    for (int i = 0; i < n; ++i) {
        if (Place(static_cast<unsigned int>(reinterpret_cast<uintptr_t>(&g_idTags[list[i].slot])), list[i]))
            ++g_stats.placed;
        else
            ++g_stats.rejected;   // all 32 GTA marker slots busy: skipped, nothing evicted
    }
}

void Reset() {
    g_ctl.Reset();
    g_playerKnown = false;
    for (uint32_t& t : g_lastGroundTry) t = 0;
}

void SetLocalStyle(const Style& style) { g_ctl.SetLocalStyle(style); }
void SetLocalPolicy(ArrivalPolicy policy, uint32_t keepMs) { g_ctl.SetLocalPolicy(policy, keepMs); }
void ClearLocal() { g_ctl.ClearLocal(); }

bool SetServerCheckpoint(int id, float x, float y, float z, bool hasZ, const Style& style,
                         ArrivalPolicy policy, uint32_t keepMs, float arrivalRadius) {
    return g_ctl.SetServer(id, Vec3{x, y, z}, hasZ, style, policy, keepMs, arrivalRadius);
}
bool RemoveServerCheckpoint(int id) { return g_ctl.RemoveServer(id); }
void ClearServerCheckpoints() { g_ctl.ClearServer(); }
bool TakeServerArrival(int& id) { return g_ctl.TakeArrival(id); }

Stats GetStats() { return g_stats; }

} // namespace skr::gps::checkpoint
