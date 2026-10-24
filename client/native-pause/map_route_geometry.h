#pragma once
// ESC MAP route geometry: dashes along the computed road polyline (screen
// space) and rectangle clipping. Pure; unit-tested in test_gps_routing.cpp.

#include <cmath>
#include <cstddef>
#include <vector>

namespace skr::native_pause::map_route {

struct Pt { float x, y; };

// Liang–Barsky: clips a–b to [min, max]; false when fully outside.
inline bool ClipToRect(Pt& a, Pt& b, Pt min, Pt max) {
    const float dx = b.x - a.x, dy = b.y - a.y;
    float t0 = 0.0f, t1 = 1.0f;
    const float p[4] = {-dx, dx, -dy, dy};
    const float q[4] = {a.x - min.x, max.x - a.x, a.y - min.y, max.y - a.y};
    for (int i = 0; i < 4; ++i) {
        if (std::fabs(p[i]) < 1e-9f) { if (q[i] < 0.0f) return false; continue; }
        const float r = q[i] / p[i];
        if (p[i] < 0.0f) { if (r > t1) return false; if (r > t0) t0 = r; }
        else             { if (r < t0) return false; if (r < t1) t1 = r; }
    }
    const Pt a0 = a;
    a = {a0.x + dx * t0, a0.y + dy * t0};
    b = {a0.x + dx * t1, a0.y + dy * t1};
    return true;
}

// Dashes of length `dash` separated by `gap` along the polyline, with the
// pattern continuing across vertices (a dash crossing a vertex bends with
// the road). Each dash is a small polyline; dashes outside [min, max] are
// dropped and partly visible ones clipped. out holds all points, counts the
// point count per dash (≥ 2).
// startPhase: distance into the dash period at pts[0] (screen px). Passing the
// on-screen length of the route already travelled keeps the pattern fixed to
// the road while the visible route start advances.
inline void BuildDashes(const Pt* pts, size_t n, float dash, float gap, Pt min, Pt max,
                        std::vector<Pt>& out, std::vector<unsigned>& counts,
                        float startPhase = 0.0f) {
    out.clear();
    counts.clear();
    if (n < 2 || dash <= 0.0f || gap < 0.0f) return;
    const float period = dash + gap;
    float phase = std::fmod(startPhase, period);   // distance into the current period
    if (!(phase >= 0.0f)) phase = 0.0f;            // negative / NaN
    std::vector<Pt> current;     // dash being built
    auto flush = [&]() {
        if (current.size() < 2) { current.clear(); return; }
        // Clip each sub-segment; keep contiguous visible runs.
        std::vector<Pt> run;
        for (size_t i = 0; i + 1 < current.size(); ++i) {
            Pt a = current[i], b = current[i + 1];
            if (!ClipToRect(a, b, min, max)) {
                if (run.size() >= 2) { out.insert(out.end(), run.begin(), run.end()); counts.push_back(static_cast<unsigned>(run.size())); }
                run.clear();
                continue;
            }
            if (run.empty() || std::fabs(run.back().x - a.x) > 1e-3f || std::fabs(run.back().y - a.y) > 1e-3f) {
                if (run.size() >= 2) { out.insert(out.end(), run.begin(), run.end()); counts.push_back(static_cast<unsigned>(run.size())); }
                run.clear();
                run.push_back(a);
            }
            run.push_back(b);
        }
        if (run.size() >= 2) { out.insert(out.end(), run.begin(), run.end()); counts.push_back(static_cast<unsigned>(run.size())); }
        current.clear();
    };
    for (size_t i = 0; i + 1 < n; ++i) {
        const Pt a = pts[i], b = pts[i + 1];
        const float len = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
        if (len < 1e-4f) continue;
        float s = 0.0f;
        while (s < len) {
            const bool inDash = phase < dash;
            const float boundary = inDash ? dash : period;
            const float step = (boundary - phase) < (len - s) ? (boundary - phase) : (len - s);
            const Pt p0 = {a.x + (b.x - a.x) * (s / len), a.y + (b.y - a.y) * (s / len)};
            const Pt p1 = {a.x + (b.x - a.x) * ((s + step) / len), a.y + (b.y - a.y) * ((s + step) / len)};
            if (inDash) {
                if (current.empty()) current.push_back(p0);
                current.push_back(p1);
            }
            s += step;
            phase += step;
            if (inDash && phase >= dash - 1e-4f) flush();
            if (phase >= period - 1e-4f) phase = 0.0f;
        }
    }
    flush();
}

} // namespace skr::native_pause::map_route
