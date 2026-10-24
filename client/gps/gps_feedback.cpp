#include "gps_feedback.h"

#include <cmath>

namespace skr::gps::feedback {
namespace {

struct Slot { Listener fn = nullptr; void* user = nullptr; };
constexpr int kMaxListeners = 4;
Slot    g_listeners[kMaxListeners];
Summary g_summary;

// Point at arc distance `along` from node i (negative = backwards).
Vec3 PointAlong(const std::vector<RouteNode>& nodes, size_t i, float along) {
    const int step = along < 0.0f ? -1 : 1;
    float left = std::fabs(along);
    size_t cur = i;
    while (left > 0.0f) {
        const long next = static_cast<long>(cur) + step;
        if (next < 0 || next >= static_cast<long>(nodes.size())) break;
        const Vec3& a = nodes[cur].pos;
        const Vec3& b = nodes[static_cast<size_t>(next)].pos;
        const float seg = a.DistanceTo2D(b);
        if (seg >= left && seg > 0.0f) {
            const float t = left / seg;
            return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
        }
        left -= seg;
        cur = static_cast<size_t>(next);
    }
    return nodes[cur].pos;
}

} // namespace

const char* ToString(EventType t) {
    switch (t) {
        case EventType::RouteStarted:  return "route-started";
        case EventType::Rerouted:      return "rerouted";
        case EventType::Recalculating: return "recalculating";
        case EventType::Arrived:       return "arrived";
        case EventType::Cancelled:     return "cancelled";
        case EventType::Unreachable:   return "unreachable";
        case EventType::Failed:        return "failed";
    }
    return "?";
}

const char* ToString(Maneuver m) {
    switch (m) {
        case Maneuver::None:        return "none";
        case Maneuver::SlightLeft:  return "slight left";
        case Maneuver::Left:        return "left";
        case Maneuver::SharpLeft:   return "sharp left";
        case Maneuver::SlightRight: return "slight right";
        case Maneuver::Right:       return "right";
        case Maneuver::SharpRight:  return "sharp right";
        case Maneuver::UTurn:       return "u-turn";
        case Maneuver::Arrive:      return "arrive";
    }
    return "?";
}

const char* ToString(Summary::State s) {
    switch (s) {
        case Summary::State::Idle:          return "idle";
        case Summary::State::Calculating:   return "calculating";
        case Summary::State::Navigating:    return "navigating";
        case Summary::State::Recalculating: return "recalculating";
        case Summary::State::Arrived:       return "arrived";
        case Summary::State::Unavailable:   return "unavailable";
    }
    return "?";
}

int Subscribe(Listener listener, void* user) {
    if (!listener) return 0;
    for (int i = 0; i < kMaxListeners; ++i)
        if (!g_listeners[i].fn) { g_listeners[i] = {listener, user}; return i + 1; }
    return 0;
}

void Unsubscribe(int id) {
    if (id >= 1 && id <= kMaxListeners) g_listeners[id - 1] = {};
}

void Emit(const Event& event) {
    for (const Slot& s : g_listeners)
        if (s.fn) s.fn(event, s.user);
}

ManeuverInfo NextManeuver(const std::vector<RouteNode>& nodes, int currentIndex, const Vec3& player,
                          LinkCountFn linkCount, float horizon) {
    ManeuverInfo info;
    if (nodes.size() < 2 || currentIndex >= static_cast<int>(nodes.size())) return info;
    const size_t first = static_cast<size_t>(currentIndex < 0 ? 0 : currentIndex);
    float along = player.DistanceTo2D(nodes[first].pos);
    bool graphKnown = false;
    for (size_t i = first; i + 1 < nodes.size(); ++i) {
        if (i > first) along += nodes[i - 1].pos.DistanceTo2D(nodes[i].pos);
        const int links = linkCount ? linkCount(nodes[i].areaId, nodes[i].nodeId) : -1;
        if (links >= 0) graphKnown = true;
        if (along > horizon) {
            info.reliable = graphKnown;   // known road, simply no turn within the horizon
            return info;
        }
        if (links < 3 || i == 0) continue;   // not a junction
        const Vec3 before = PointAlong(nodes, i, -25.0f);
        const Vec3 after  = PointAlong(nodes, i, 25.0f);
        const Vec3& at = nodes[i].pos;
        const float inX = at.x - before.x, inY = at.y - before.y;
        const float outX = after.x - at.x, outY = after.y - at.y;
        if ((inX == 0 && inY == 0) || (outX == 0 && outY == 0)) continue;
        // Signed angle in degrees; positive = counter-clockwise = left (north-up, y north).
        const float angle = std::atan2(inX * outY - inY * outX, inX * outX + inY * outY) * 57.2957795f;
        const float mag = std::fabs(angle);
        if (mag < 25.0f) continue;
        const bool left = angle > 0.0f;
        info.maneuver = mag >= 160.0f ? Maneuver::UTurn
                      : mag >= 120.0f ? (left ? Maneuver::SharpLeft : Maneuver::SharpRight)
                      : mag >= 60.0f  ? (left ? Maneuver::Left : Maneuver::Right)
                                      : (left ? Maneuver::SlightLeft : Maneuver::SlightRight);
        info.distance = along;
        info.reliable = true;
        return info;
    }
    // No turn ahead: arrival is the next thing, if within the horizon.
    float toEnd = player.DistanceTo2D(nodes[first].pos);
    for (size_t i = first + 1; i < nodes.size(); ++i) toEnd += nodes[i - 1].pos.DistanceTo2D(nodes[i].pos);
    if (graphKnown && toEnd <= horizon) {
        info.maneuver = Maneuver::Arrive;
        info.distance = toEnd;
        info.reliable = true;
    }
    return info;
}

const Summary& GetSummary() { return g_summary; }
void SetSummary(const Summary& summary) { g_summary = summary; }

} // namespace skr::gps::feedback
