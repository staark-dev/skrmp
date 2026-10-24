#pragma once
// ═══════════════════════════════════════════════════════════════════════
// GPS Destination Manager — waypoint tracking, validation, arrival
// ═══════════════════════════════════════════════════════════════════════
// Reads the GTA map waypoint blip from CRadar::ms_RadarTrace and exposes
// it as a GPS destination. Also supports programmatic destinations set
// via SetDestination(). All functions run on the GTA thread.

#include "gps_types.h"

namespace skr::gps {

// Check if the player has placed a waypoint on the GTA map.
// Reads CRadar::ms_RadarTrace for RADAR_SPRITE_WAYPOINT (41).
bool HasMapWaypoint();

// Returns the world position of the current map waypoint.
// Only valid when HasMapWaypoint() returns true.
Vec3 GetMapWaypointPosition();

// Removes the GTA map waypoint through CRadar::ClearBlip (0x587CE0): the
// handle tracked in CMenuManager::m_nTargetBlipIndex, then any other
// sprite-41 blip. Returns true when no waypoint remains afterwards.
bool ClearMapWaypoint();

// Set a GPS destination (from map waypoint or programmatic).
// Passing the same position again is a no-op.
void SetDestination(const Vec3& pos);

// Clear the current GPS destination and cancel navigation.
void ClearDestination();

// True if a GPS destination is currently set.
bool HasDestination();

// Returns the current destination position.
// Only valid when HasDestination() returns true.
Vec3 GetDestination();

// Arrival radius in world units (default 15.0).
void  SetArrivalRadius(float radius);
float GetArrivalRadius();

// Check if the player is within the arrival radius of the destination.
bool IsArrived(const Vec3& playerPos);

} // namespace skr::gps
