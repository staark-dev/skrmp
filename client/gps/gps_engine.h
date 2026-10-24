#pragma once
// ═══════════════════════════════════════════════════════════════════════
// GPS Navigation Engine — integration layer
// ═══════════════════════════════════════════════════════════════════════
// Connects destination manager, route planner and GTA state into a
// single per-frame tick. Called from game_state.cpp Tick().

#include "gps_types.h"
#include "gps_route.h"

namespace skr::gps {

// Call once per GTA frame from game_state::Tick().
// Reads the map waypoint, manages route lifecycle, detects arrival/deviation.
void Tick(float playerX, float playerY, float playerZ);

// Waypoint/route sync only (no arrival or deviation handling). Called by the
// ESC MAP every frame so a waypoint placed there gets its route immediately,
// even if CGame::Process does not reach Tick while the game is paused.
void SyncWaypoint(float playerX, float playerY, float playerZ);

// Reset all GPS state (on disconnect / game exit).
void Reset();

// Client shutdown: reset and remove the adaptive-zoom call-site patch.
void Shutdown();

// Read-only access for future renderers.
const RouteSnapshot& GetRoute();
NavStatus             GetStatus();

// Where the engine reads and clears the map waypoint. Defaults to GTA's
// CRadar (gps_destination.h); tests install a fake. nullptr restores GTA.
struct WaypointSource {
    bool (*has)();
    Vec3 (*pos)();
    bool (*clear)();
};
void SetWaypointSourceForTesting(const WaypointSource* src);

} // namespace skr::gps
