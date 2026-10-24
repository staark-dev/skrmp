#pragma once
// ═══════════════════════════════════════════════════════════════════════
// GPS Route Renderer — D3D9 minimap route line
// ═══════════════════════════════════════════════════════════════════════
// Draws the active GPS route on the minimap using Direct3D9 primitives.
// Called from HookedDrawRadar after the GTA trampoline and before scissor
// restore, so Square/Custom modes get scissor clipping for free.
// All functions run on the GTA render thread.

#include <cstdint>

namespace skr::gps {

// Draw the current GPS route on the minimap.
// Called from HookedDrawRadar after the trampoline.
// radarX/Y/W/H are the measured screen-pixel radar rect.
// isSquare: true for Square/Custom modes (scissor already active).
void RenderRoute(float radarX, float radarY, float radarW, float radarH, bool isSquare);

// Clear cached render state (call on D3D9 Reset / disconnect).
void ResetRenderer();

// Counters for the engine's periodic summary (the render path never logs).
struct RenderStats {
    uint32_t frames;           // route frames drawn
    uint32_t failures;         // state block / draw call / device failures
    uint32_t transformFaults;  // GTA radar transform faulted (SEH)
    uint32_t pieces;           // visible pieces in the last frame
};
RenderStats GetRenderStats();

// Radar rect (screen px) from the latest DrawRadar, for HUD text placement.
bool LastRadarRect(float& x, float& y, float& w, float& h);

// Radar rect of the latest DrawRadar plus a sequence number that advances once
// per radar draw. The ImGui navigation panel (EndScene) only draws when the
// sequence moved since its previous frame, i.e. GTA really drew the minimap
// this frame (hidden HUD, cutscenes and extra EndScene passes draw nothing).
struct RadarFrame {
    float    x, y, w, h;
    bool     square;     // Square/Custom radar style
    bool     valid;
    uint32_t sequence;
};
RadarFrame LastRadarFrame();

// Fallback navigation text above the minimap (CFont, HUD 2D pass), used only
// while the ImGui navigation panel cannot render (gps_panel.h). Draws only
// when Renderer=1 and there is something to say (never in Idle).
void DrawStatus();

} // namespace skr::gps
