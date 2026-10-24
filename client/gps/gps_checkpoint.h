#pragma once
// ═══════════════════════════════════════════════════════════════════════
// GPS world checkpoint — GTA side (API-003.3)
// ═══════════════════════════════════════════════════════════════════════
// A 3D cylinder at the destination, drawn by GTA's own C3dMarkers (no render
// hook). GTA keeps a 3D marker only for the frame it was placed in
// (C3dMarkers::Render 0x725040 clears m_bMustBeRenderedThisFrame and frees
// unplaced ones), so PlaceFrame() runs every CGame::Process frame from the
// existing game_state tick. Marker ids are addresses inside SKR.Client.dll,
// so they can never match a GTA, script or SA-MP marker id; the cylinder type
// never evicts another owner's marker when GTA's 32 slots are full
// (PlaceMarker 0x725120 returns null, verified on the clean 1.0 US exe).
// State and policies: gps_checkpoint_model.h. GTA thread only.

#include "gps_checkpoint_model.h"

namespace skr::gps::checkpoint {

// Game tick (≈10 Hz, after gps::Tick): follows the local waypoint, resolves
// ground heights near the player, detects advisory server arrivals.
void Tick(float playerX, float playerY, float playerZ);

// Every CGame::Process frame: places the visible markers (no allocation, no I/O).
void PlaceFrame();

void Reset();   // disconnect: removes every marker

// Configuration (also the hooks for the API-003.3 Pawn natives, not wired yet).
void SetLocalStyle(const Style& style);
void SetLocalPolicy(ArrivalPolicy policy, uint32_t keepMs);
void ClearLocal();
bool SetServerCheckpoint(int id, float x, float y, float z, bool hasZ, const Style& style,
                         ArrivalPolicy policy, uint32_t keepMs, float arrivalRadius);
bool RemoveServerCheckpoint(int id);
void ClearServerCheckpoints();
bool TakeServerArrival(int& id);   // advisory only — the server must validate

struct Stats { uint32_t placed, rejected, groundQueries, groundFaults; };
Stats GetStats();

} // namespace skr::gps::checkpoint
