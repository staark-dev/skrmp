#pragma once
// Polyline clipping in GTA radar space (CRadar::TransformRealWorldPointToRadarSpace):
// the visible radar is the unit circle (round radar) or the unit square
// [-1,1]² (square/custom radar). Pure; unit-tested in test_gps_routing.cpp.

#include <cmath>
#include <cstdint>
#include <vector>

namespace skr::gps::clip {

struct P2 { float x, y; };

enum class Region : uint8_t { UnitCircle, UnitSquare };

// Parametric interval [t0, t1] ⊆ [0,1] of segment a→b inside the region.
inline bool ClipSegment(P2 a, P2 b, Region region, float& t0, float& t1) {
    const float dx = b.x - a.x, dy = b.y - a.y;
    t0 = 0.0f; t1 = 1.0f;
    if (region == Region::UnitCircle) {
        const float A = dx * dx + dy * dy;
        const float B = 2.0f * (a.x * dx + a.y * dy);
        const float C = a.x * a.x + a.y * a.y - 1.0f;
        if (A < 1e-12f) return C <= 0.0f;
        const float disc = B * B - 4.0f * A * C;
        if (disc < 0.0f) return false;
        const float s = std::sqrt(disc);
        const float r0 = (-B - s) / (2.0f * A), r1 = (-B + s) / (2.0f * A);
        t0 = r0 > 0.0f ? r0 : 0.0f;
        t1 = r1 < 1.0f ? r1 : 1.0f;
        return t0 < t1;
    }
    // Liang–Barsky against x,y ∈ [-1, 1].
    const float p[4] = {-dx, dx, -dy, dy};
    const float q[4] = {a.x + 1.0f, 1.0f - a.x, a.y + 1.0f, 1.0f - a.y};
    for (int i = 0; i < 4; ++i) {
        if (std::fabs(p[i]) < 1e-12f) { if (q[i] < 0.0f) return false; continue; }
        const float r = q[i] / p[i];
        if (p[i] < 0.0f) { if (r > t1) return false; if (r > t0) t0 = r; }
        else             { if (r < t0) return false; if (r < t1) t1 = r; }
    }
    return t0 < t1;
}

// Clips a polyline; consecutive inside pieces stay joined. Output: flat point
// list plus the point count of each piece (every piece has ≥ 2 points).
inline void ClipPolyline(const P2* pts, size_t n, Region region,
                         std::vector<P2>& outPts, std::vector<uint32_t>& outCounts) {
    outPts.clear();
    outCounts.clear();
    bool open = false;
    for (size_t i = 0; i + 1 < n; ++i) {
        float t0, t1;
        if (!ClipSegment(pts[i], pts[i + 1], region, t0, t1)) { open = false; continue; }
        const P2 a = {pts[i].x + (pts[i + 1].x - pts[i].x) * t0, pts[i].y + (pts[i + 1].y - pts[i].y) * t0};
        const P2 b = {pts[i].x + (pts[i + 1].x - pts[i].x) * t1, pts[i].y + (pts[i + 1].y - pts[i].y) * t1};
        if (!open || t0 > 0.0f) {          // a new piece starts here
            outPts.push_back(a);
            outCounts.push_back(1);
            open = true;
        }
        outPts.push_back(b);
        ++outCounts.back();
        if (t1 < 1.0f) open = false;       // left the region inside this segment
    }
}

} // namespace skr::gps::clip
