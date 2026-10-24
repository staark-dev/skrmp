#pragma once
#include <cstdint>
#include <cmath>

namespace skr::gps {

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;

    float DistanceTo(const Vec3& o) const {
        float dx = x - o.x, dy = y - o.y, dz = z - o.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }
    float DistanceTo2D(const Vec3& o) const {
        float dx = x - o.x, dy = y - o.y;
        return std::sqrt(dx * dx + dy * dy);
    }
};

enum class NavStatus : uint8_t {
    Idle         = 0,   // no destination set
    Calculating  = 1,   // pathfind queued / in progress
    Navigating   = 2,   // valid route, following it
    Arrived      = 3,   // within arrival radius
    Unreachable  = 4,   // pathfind completed with no route
    Failed       = 5,   // runtime error (stale nodes, crash guard)
};

struct RouteNode {
    Vec3  pos;
    int16_t areaId  = -1;
    int16_t nodeId  = -1;
};

} // namespace skr::gps
