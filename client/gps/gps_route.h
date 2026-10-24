#pragma once
// ═══════════════════════════════════════════════════════════════════════
// GPS Route Planner + Route State
// ═══════════════════════════════════════════════════════════════════════
// Calculates a vehicle road route using CPathFind::DoPathSearch and
// maintains the cached route + navigation state. All functions run on
// the GTA thread — no worker-thread game access.

#include "gps_types.h"
#include <vector>

namespace skr::gps {

// Native DoPathSearch result buffer. A native result that fills it is treated
// as truncated and replaced by a road-graph route.
static constexpr int kMaxRouteNodes = 64;

// Straight-line distance up to which the native search is tried first.
static constexpr float kNativeRange = 500.0f;

enum class RouteSource : uint8_t { None, Native, Graph };

// Why a route is missing (status Unreachable / Failed / Calculating).
enum class RouteFailure : uint8_t {
    None,
    NoRoadNearOrigin,       // player > 300 from any road (interior, off-map): retried
    NoRoadNearDestination,  // waypoint > 300 from any road
    Disconnected,           // no road reachable from the player's road network
    NoPath,                 // search exhausted
    RoadDataUnavailable,    // nodes*.dat could not be read/validated
    RoadDataLoading,        // still loading (Pending)
    Timeout,                // road data not ready in time
};
const char* ToString(RouteFailure f);

struct RouteSnapshot {
    NavStatus             status        = NavStatus::Idle;
    RouteSource           source        = RouteSource::None;
    RouteFailure          failure       = RouteFailure::None;
    std::vector<RouteNode> nodes;                    // complete route on road geometry, origin → destination
    float                 totalDistance  = 0.0f;
    float                 remainingDist = 0.0f;
    int                   currentIndex  = 0;        // index of next node to reach
    Vec3                  destination   = {};       // requested (waypoint) position
    Vec3                  roadDestination = {};     // where the route ends on the road
    float                 destinationSnap = 0.0f;   // waypoint → road end distance (off-road gap)
};

// Found       → status Navigating, complete route.
// Pending     → status Calculating: road graph still loading; call again later.
// Unreachable → status Unreachable: no road near an endpoint, or no connection.
// Failed      → status Failed: road data unavailable or a search faulted.
// No outcome other than Found ever leaves route nodes behind.
enum class RouteOutcome : uint8_t { Found, Pending, Unreachable, Failed };

// Local destinations (≤ kNativeRange) use CPathFind::DoPathSearch when its
// result is complete and ends at the destination; everything else uses the
// road graph (gps_graph.h).
RouteOutcome CalculateRoute(const Vec3& origin, const Vec3& target);

// Tests: replace the native search. Returns node count, -1 on fault.
using NativeSearchFn = int (*)(const Vec3& origin, const Vec3& target, std::vector<RouteNode>& out);
void SetNativeSearchForTesting(NativeSearchFn fn);   // nullptr restores DoPathSearch

// Invalidate the current route (e.g. on destination change or cancellation).
void InvalidateRoute();

// Drop any route and report `status` (Failed / Unreachable) for the kept destination.
void AbandonRoute(NavStatus status, RouteFailure failure);

// Returns a read-only snapshot of the current route state.
const RouteSnapshot& GetRouteSnapshot();

// Returns the current navigation status.
NavStatus GetNavStatus();

// Advance navigation: update currentIndex, remainingDist based on player position.
// Call once per frame. Returns true if the player crossed a node checkpoint.
bool UpdateNavigation(const Vec3& playerPos);

// Returns true if the player has deviated significantly from the route.
// deviationThreshold is in world units (default 40.0).
bool HasDeviated(const Vec3& playerPos, float deviationThreshold = 40.0f);

} // namespace skr::gps
