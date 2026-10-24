#pragma once
// ESC MAP route strokes: thin navy underlay + short lime dashes + dotted
// off-road connector, drawn into an ImGui draw list. Shared by
// pause_renderer.cpp (DrawMapRoute) and the offscreen GPS preview so the
// screenshots show the production stroke code.
//
// Widths are screen-space pixels from the back-buffer height, never from the
// map zoom, so zooming the map changes the route's length, not its weight.

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "native-pause/map_route_geometry.h"

namespace skr::native_pause::map_route {

struct MapRouteStyle {
    float dashWidth;      // lime dash, final width in px
    float underlayWidth;  // continuous navy band under the dashes
    float dash, gap;      // dash rhythm in px
    float dotRadius, dotSpacing;   // off-road connector
    ImU32 dashColor, underlayColor, dotColor;
};

// 1.7 px dashes on a 2.7 px underlay at 768 p, at most 15 % heavier at 1080 p
// and never above 2.0 px for the dash.
inline MapRouteStyle StyleForScreen(float screenHeight) {
    float k = screenHeight > 0.0f ? screenHeight / 768.0f : 1.0f;
    k = k < 1.0f ? 1.0f : (k > 1.15f ? 1.15f : k);
    MapRouteStyle s;
    s.dashWidth     = (std::min)(1.7f * k, 2.0f);
    s.underlayWidth = s.dashWidth + 1.0f;
    s.dash          = 7.0f * k;
    s.gap           = 5.0f * k;
    s.dotRadius     = 1.1f * k;
    s.dotSpacing    = 6.0f * k;
    s.dashColor     = IM_COL32(163, 230, 91, 235);   // #A3E65B, minimap route colour
    s.underlayColor = IM_COL32(12, 24, 38, 120);
    s.dotColor      = IM_COL32(163, 230, 91, 150);
    return s;
}

// pts: remaining road route in screen space. startPhase: on-screen length of
// the route already travelled (see BuildDashes). clipMin/clipMax: map rect.
inline void DrawRouteStrokes(ImDrawList* dl, const ImVec2* pts, int n, float startPhase,
                             ImVec2 clipMin, ImVec2 clipMax, ImVec2 roadEnd, ImVec2 waypoint,
                             const MapRouteStyle& st) {
    if (!dl || n < 2) return;
    static std::vector<Pt> dashPts;
    static std::vector<unsigned> dashCounts;

    // ImGui clips the underlay with the draw list's clip rect; dashes are
    // clipped geometrically (map rect + 1 px) so none straddles the edge.
    dl->PushClipRect(clipMin, clipMax, true);
    dl->AddPolyline(pts, n, st.underlayColor, ImDrawFlags_None, st.underlayWidth);

    BuildDashes(reinterpret_cast<const Pt*>(pts), static_cast<size_t>(n), st.dash, st.gap,
                {clipMin.x - 1.0f, clipMin.y - 1.0f}, {clipMax.x + 1.0f, clipMax.y + 1.0f},
                dashPts, dashCounts, startPhase);
    size_t at = 0;
    for (unsigned c : dashCounts) {
        dl->AddPolyline(reinterpret_cast<const ImVec2*>(dashPts.data() + at), static_cast<int>(c),
                        st.dashColor, ImDrawFlags_None, st.dashWidth);
        at += c;
    }

    // Off-road gap: road end → waypoint, dotted and dimmer than the road route.
    const float gx = waypoint.x - roadEnd.x, gy = waypoint.y - roadEnd.y;
    const float gapLen = std::sqrt(gx * gx + gy * gy);
    if (gapLen > st.dotSpacing * 1.5f) {
        const int dots = static_cast<int>(gapLen / st.dotSpacing);
        for (int i = 1; i < dots; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(dots);
            dl->AddCircleFilled(ImVec2(roadEnd.x + gx * t, roadEnd.y + gy * t), st.dotRadius, st.dotColor, 8);
        }
    }
    dl->PopClipRect();
}

} // namespace skr::native_pause::map_route
