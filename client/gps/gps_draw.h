#pragma once
// GPS route polyline draw — pure D3D9, no GTA dependencies (unit-testable).
//
// RenderWare caches FVF / vertex declaration / shaders / stream source and
// skips redundant Set* calls (rwcore.h _rwD3D9SetFVFMacro etc.), so any
// device state changed here must be returned exactly as found. A D3DSBT_ALL
// state block is captured before the first change and applied before return.

#include <d3d9.h>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace skr::gps {

struct ScreenPoint { float x, y; };

// Stroke widths are FINAL displayed widths in screen pixels (not half-widths).
// Each stroke is a solid core of (width - kFringe) plus a kFringe-wide alpha
// fade on both sides (no MSAA in GTA), so 50 % coverage sits at width / 2.
constexpr float kFringe = 1.0f;

struct RouteStyle {
    float fillWidth;      // lime route
    float outlineWidth;   // dark separation stroke under it
    DWORD fillColor;      // ARGB
    DWORD outlineColor;   // ARGB
};

// Minimap style for a back buffer `screenHeight` px tall: 2.0 px route /
// 2.9 px outline at 768 p, growing at most 15 % (2.3 / 3.2 px at 1080 p) so
// the visual weight stays close across resolutions without going thick.
RouteStyle MinimapRouteStyle(float screenHeight);

// Draws the outline and fill strips through `pts` with the minimap style.
// clip: optional scissor rect (nullptr = leave the caller's scissor state).
// Returns false and changes nothing if the state block cannot be created.
bool DrawRouteStrip(IDirect3DDevice9* dev, const ScreenPoint* pts, size_t count,
                    const RECT* clip);

// Several pieces (a clipped route), one state block. counts[i] points each.
bool DrawRouteStrips(IDirect3DDevice9* dev, const ScreenPoint* pts, const uint32_t* counts,
                     size_t pieces, const RECT* clip);
bool DrawRouteStrips(IDirect3DDevice9* dev, const ScreenPoint* pts, const uint32_t* counts,
                     size_t pieces, const RECT* clip, const RouteStyle& style);

// Triangle-strip outline of a polyline (pairs: left, right per emitted point).
// Joins are mitred while the miter stays within kMiterLimit × halfWidth;
// sharper corners become bevels (two pairs at the corner, one per segment
// normal), so no spike ever leaves the road. Near-duplicate points (< 0.5 px)
// are dropped first. The emitted pair count depends only on the polyline,
// never on halfWidth, so strips of different widths line up pair by pair.
constexpr float kMiterLimit = 2.0f;
void BuildStripOutline(const ScreenPoint* pts, size_t n, float halfWidth, std::vector<ScreenPoint>& out);

} // namespace skr::gps
