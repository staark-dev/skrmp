#pragma once
// ═══════════════════════════════════════════════════════════════════════
// GPS navigation feedback — events, summary, maneuvers (no GTA access)
// ═══════════════════════════════════════════════════════════════════════
// The engine emits NavEvents and refreshes the Summary; UI consumers read the
// Summary each frame or subscribe to events. A notification renderer (e.g.
// the Codex C3 Native API UI) can subscribe here without coupling to GPS.

#include "gps_types.h"
#include "gps_route.h"
#include <cstdint>
#include <vector>

namespace skr::gps::feedback {

enum class EventType : uint8_t {
    RouteStarted,     // first route for a new destination
    Rerouted,         // new route after leaving the recommended road
    Recalculating,    // off-route detected, new route requested
    Arrived,
    Cancelled,        // waypoint removed by the player
    Unreachable,
    Failed,
};
const char* ToString(EventType t);

struct Event {
    EventType    type = EventType::RouteStarted;
    RouteFailure failure = RouteFailure::None;
    float        distance = 0.0f;   // route length (started/rerouted) or remaining
    uint32_t     tick = 0;
};

using Listener = void (*)(const Event& event, void* user);
int  Subscribe(Listener listener, void* user);   // returns id > 0, or 0 when full (max 4)
void Unsubscribe(int id);
void Emit(const Event& event);                    // engine only (GTA thread)

enum class Maneuver : uint8_t {
    None, SlightLeft, Left, SharpLeft, SlightRight, Right, SharpRight, UTurn, Arrive,
};
const char* ToString(Maneuver m);

struct ManeuverInfo {
    Maneuver maneuver = Maneuver::None;
    float    distance = 0.0f;   // along the route from the player
    bool     reliable = false;  // based on road-graph data (None + reliable = no turn within the horizon)
};

// Link count of a route node in the road graph, or -1 when unknown
// (projection points, no graph). Junction = 3+ links.
using LinkCountFn = int (*)(int16_t area, int16_t node);

// Next turn at a real junction within `horizon` metres along the route.
// Angles use points ~25 m before/after the junction (curve tolerant):
// <25° none, 25–60 slight, 60–120 turn, 120–160 sharp, ≥160 U-turn.
ManeuverInfo NextManeuver(const std::vector<RouteNode>& nodes, int currentIndex, const Vec3& player,
                          LinkCountFn linkCount, float horizon = 1500.0f);

struct Summary {
    enum class State : uint8_t { Idle, Calculating, Navigating, Recalculating, Arrived, Unavailable };
    State        state = State::Idle;
    RouteFailure failure = RouteFailure::None;
    float        remaining = 0.0f;
    ManeuverInfo next;
    uint32_t     stateSince = 0;   // GetTickCount() when `state` was entered
};
const char* ToString(Summary::State s);

const Summary& GetSummary();
void SetSummary(const Summary& summary);   // engine only

} // namespace skr::gps::feedback
