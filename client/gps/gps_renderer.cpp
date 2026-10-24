#include "gps_renderer.h"
#include "gps_clip.h"
#include "gps_draw.h"
#include "gps_engine.h"
#include "gps_flags.h"
#include "gps_route.h"
#include <d3d9.h>
#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <vector>

namespace skr::gps {

// ── GTA addresses (SA 1.0 US) ──────────────────────────────────────
static constexpr uintptr_t kRwD3D9Device                    = 0xC97C28;
static constexpr uintptr_t kTransformRealWorldToRadarSpace   = 0x583530;
static constexpr uintptr_t kTransformRadarPointToScreenSpace = 0x583480;
static constexpr uintptr_t kRsMaxHeight                     = 0xC17048;

struct V2 { float x, y; };
using TransformFn = void(__cdecl*)(V2*, const V2*);

// ── Diagnostics: counters only (no file I/O on the render path) ────
static std::atomic<uint32_t> g_frames{0}, g_failures{0}, g_transformFaults{0};
static std::atomic<uint32_t> g_lastPieces{0};

// ── Per-frame scratch (render thread only) ──────────────────────────
static std::vector<clip::P2>   g_radarPts;
static std::vector<clip::P2>   g_clipped;
static std::vector<uint32_t>   g_counts;
static std::vector<ScreenPoint> g_screen;
static std::vector<ScreenPoint> g_smooth;
static std::vector<uint32_t>   g_smoothCounts;

// World → radar space for nodes [first, end). SEH-guarded GTA calls; no C++
// objects with destructors live in this function.
static bool ToRadarSpace(const RouteNode* nodes, size_t count, clip::P2* out) {
    auto worldToRadar = reinterpret_cast<TransformFn>(kTransformRealWorldToRadarSpace);
    __try {
        for (size_t i = 0; i < count; ++i) {
            V2 world{nodes[i].pos.x, nodes[i].pos.y}, radar{};
            worldToRadar(&radar, &world);
            out[i] = {radar.x, radar.y};
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool ToScreenSpace(const clip::P2* in, size_t count, ScreenPoint* out) {
    auto radarToScreen = reinterpret_cast<TransformFn>(kTransformRadarPointToScreenSpace);
    __try {
        for (size_t i = 0; i < count; ++i) {
            V2 radar{in[i].x, in[i].y}, screen{};
            radarToScreen(&screen, &radar);
            out[i] = {screen.x, screen.y};
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The radar is centred on the player: radar origin (0,0). Replace the first
// point with the player's projection onto the first segment so no stub is
// drawn behind the player.
static void TrimBehindPlayer(std::vector<clip::P2>& pts) {
    if (pts.size() < 2) return;
    const clip::P2 a = pts[0], b = pts[1];
    const float dx = b.x - a.x, dy = b.y - a.y, len2 = dx * dx + dy * dy;
    if (len2 < 1e-10f) return;
    float t = (-a.x * dx + -a.y * dy) / len2;
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    pts[0] = {a.x + dx * t, a.y + dy * t};
}

// Chaikin corner-cutting: one subdivision pass replaces each inner pair of
// points with two new points at 25 % and 75 % along the segment, producing a
// smoother polyline that stays close to the original path. Two iterations are
// enough for road-like geometry; endpoints are preserved so the route still
// starts/ends at the correct screen positions.
static void ChaikinSmooth(const ScreenPoint* in, size_t n, int iterations,
                          std::vector<ScreenPoint>& out) {
    static std::vector<ScreenPoint> tmp;
    out.assign(in, in + n);
    for (int iter = 0; iter < iterations && out.size() >= 3; ++iter) {
        tmp.clear();
        tmp.reserve(out.size() * 2);
        tmp.push_back(out.front());
        for (size_t i = 0; i + 1 < out.size(); ++i) {
            const float ax = out[i].x, ay = out[i].y;
            const float bx = out[i + 1].x, by = out[i + 1].y;
            tmp.push_back({ax * 0.75f + bx * 0.25f, ay * 0.75f + by * 0.25f});
            tmp.push_back({ax * 0.25f + bx * 0.75f, ay * 0.25f + by * 0.75f});
        }
        tmp.push_back(out.back());
        out.swap(tmp);
    }
}

static float    g_rectX = 0, g_rectY = 0, g_rectW = 0, g_rectH = 0;
static bool     g_rectKnown = false;
static bool     g_rectSquare = false;
static uint32_t g_rectSequence = 0;

bool LastRadarRect(float& x, float& y, float& w, float& h) {
    x = g_rectX; y = g_rectY; w = g_rectW; h = g_rectH;
    return g_rectKnown;
}

RadarFrame LastRadarFrame() {
    return {g_rectX, g_rectY, g_rectW, g_rectH, g_rectSquare, g_rectKnown, g_rectSequence};
}

void RenderRoute(float radarX, float radarY, float radarW, float radarH, bool isSquare) {
    g_rectX = radarX; g_rectY = radarY; g_rectW = radarW; g_rectH = radarH;
    g_rectKnown = radarW > 0.0f && radarH > 0.0f;
    g_rectSquare = isSquare;
    ++g_rectSequence;
    if (!flags::RendererEnabled()) return;
    if (GetStatus() != NavStatus::Navigating) return;
    const RouteSnapshot& snap = GetRoute();
    if (snap.nodes.size() < 2) return;

    const size_t first = static_cast<size_t>((std::max)(0, snap.currentIndex - 1));
    if (first + 2 > snap.nodes.size()) return;
    const size_t count = snap.nodes.size() - first;

    g_radarPts.resize(count);
    if (!ToRadarSpace(snap.nodes.data() + first, count, g_radarPts.data())) {
        g_transformFaults.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    TrimBehindPlayer(g_radarPts);
    clip::ClipPolyline(g_radarPts.data(), g_radarPts.size(),
                       isSquare ? clip::Region::UnitSquare : clip::Region::UnitCircle, g_clipped, g_counts);
    g_lastPieces.store(static_cast<uint32_t>(g_counts.size()), std::memory_order_relaxed);
    if (g_counts.empty()) return;   // route entirely outside the radar view

    g_screen.resize(g_clipped.size());
    if (!ToScreenSpace(g_clipped.data(), g_clipped.size(), g_screen.data())) {
        g_transformFaults.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // Smooth each clipped piece independently so corners follow road geometry
    // more naturally. Two Chaikin iterations soften zigzags without departing
    // from the road corridor (the route nodes are at intersections/waypoints).
    g_smooth.clear();
    g_smoothCounts.clear();
    size_t off = 0;
    for (size_t k = 0; k < g_counts.size(); ++k) {
        const size_t n = g_counts[k];
        if (n >= 3) {
            const size_t before = g_smooth.size();
            static std::vector<ScreenPoint> piece;
            ChaikinSmooth(g_screen.data() + off, n, 2, piece);
            g_smooth.insert(g_smooth.end(), piece.begin(), piece.end());
            g_smoothCounts.push_back(static_cast<uint32_t>(g_smooth.size() - before));
        } else if (n >= 2) {
            g_smooth.push_back(g_screen[off]);
            g_smooth.push_back(g_screen[off + 1]);
            g_smoothCounts.push_back(2);
        }
        off += n;
    }
    if (g_smoothCounts.empty()) return;

    IDirect3DDevice9* dev = *reinterpret_cast<IDirect3DDevice9**>(kRwD3D9Device);
    if (!dev) { g_failures.fetch_add(1, std::memory_order_relaxed); return; }

    // Geometry is already clipped to the radar shape; the bounding-box scissor
    // only guards line width at the round radar's edge. Square/Custom keep the
    // scissor HookedDrawRadar set.
    RECT radarScissor{};
    if (!isSquare) {
        radarScissor.left   = static_cast<LONG>(radarX);
        radarScissor.top    = static_cast<LONG>(radarY);
        radarScissor.right  = static_cast<LONG>(std::ceil(radarX + radarW));
        radarScissor.bottom = static_cast<LONG>(std::ceil(radarY + radarH));
    }
    const RouteStyle style = MinimapRouteStyle(static_cast<float>(*reinterpret_cast<int*>(kRsMaxHeight)));
    if (DrawRouteStrips(dev, g_smooth.data(), g_smoothCounts.data(), g_smoothCounts.size(),
                        isSquare ? nullptr : &radarScissor, style))
        g_frames.fetch_add(1, std::memory_order_relaxed);
    else
        g_failures.fetch_add(1, std::memory_order_relaxed);
}

RenderStats GetRenderStats() {
    return {g_frames.load(std::memory_order_relaxed), g_failures.load(std::memory_order_relaxed),
            g_transformFaults.load(std::memory_order_relaxed), g_lastPieces.load(std::memory_order_relaxed)};
}

void ResetRenderer() {
    g_radarPts.clear();
    g_clipped.clear();
    g_counts.clear();
    g_screen.clear();
    g_lastPieces.store(0, std::memory_order_relaxed);
}

} // namespace skr::gps
