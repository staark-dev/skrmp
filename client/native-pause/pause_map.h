#pragma once
// ═══════════════════════════════════════════════════════════════════════
// SKR Native Pause Menu — Embedded GTA San Andreas map (MAP tab)
// ═══════════════════════════════════════════════════════════════════════
// Draws the real radar tiles inside the native ESC menu. Tiles are read
// from <gta>\models\gta3.img (radar00.txd … radar143.txd), decoded from
// their RenderWare texture-native data and uploaded as our own D3D9
// textures — GTA's streaming and frontend state are not touched.
//
// World mapping (GTA SA, CRadar::DrawRadarMap 0x586880 /
// RequestMapSection 0x584B50): 12 x 12 sections of 500 x 500 units,
// world X -3000 → +3000 west→east, world Y +3000 → -3000 north→south,
// texture index = x + 12 * y (y = 0 is the northern row).
//
// All functions run on the GTA render thread (inside the ESC frame).

#include <d3d9.h>
#include <imgui.h>

namespace skr::native_pause::map {

constexpr float kWorldMin  = -3000.0f;
constexpr float kWorldMax  =  3000.0f;
constexpr float kWorldSize =  6000.0f;
constexpr int   kTilesPerSide = 12;
constexpr int   kTileCount    = kTilesPerSide * kTilesPerSide;

// Continue lazy loading (a few tiles per call) with the current device.
void Update(IDirect3DDevice9* device);

// Device lifetime hooks (called by the renderer).
void OnDeviceChanged();   // release textures created on the old device
void OnBeforeReset();     // release D3DPOOL_DEFAULT textures (if that pool is in use)
void Shutdown();

// Load status for the UI.
int  LoadedTiles();
bool Finished();          // every tile attempted
const char* Error();      // non-null when the archive could not be used

// Draws all tiles into [min, max] for a view centred on world (cx, cy) at
// `zoom` screen pixels per world unit. Caller sets the clip rect.
void DrawTiles(ImDrawList* dl, ImVec2 min, ImVec2 max, float cx, float cy, float zoom);

// World → screen for the same view.
ImVec2 WorldToScreen(ImVec2 min, ImVec2 max, float cx, float cy, float zoom, float wx, float wy);

// Local player's world position and forward (heading) vector, read from the
// player's vehicle if in one, otherwise the ped (CPlaceable matrix).
bool GetPlayerPose(float& x, float& y, float& fx, float& fy);

// Local player's world position including z (vehicle if in one, else ped).
bool GetPlayerPosition(float& x, float& y, float& z);

// ── Waypoint management (GTA CRadar native API) ──────────────────────
// Place a waypoint at (worldX, worldY) using CRadar::SetCoordBlip +
// SetBlipSprite. Removes any existing waypoint first.
void SetWaypoint(float worldX, float worldY);

// Remove the current map waypoint, if any.
void ClearWaypoint();

// True if a waypoint blip (sprite 41) exists in CRadar::ms_RadarTrace.
bool HasWaypoint();

// Returns the world position of the current waypoint.
// Only valid when HasWaypoint() returns true.
void GetWaypointPos(float& wx, float& wy);

// Screen → world inverse of WorldToScreen for the same view parameters.
void ScreenToWorld(ImVec2 min, ImVec2 max, float cx, float cy, float zoom,
                   float sx, float sy, float& wx, float& wy);

} // namespace skr::native_pause::map
