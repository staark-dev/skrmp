#pragma once
// ═══════════════════════════════════════════════════════════════════════
// SKR Native HUD — health/armour bars under the radar (GTA V-style)
// ═══════════════════════════════════════════════════════════════════════
// Drawn in GTA's own 2D HUD pass (right after CHud::Draw) with
// CSprite2d::DrawRect / CFont, never inside CHud::DrawRadar, so radar tiles,
// mask, blips and the player arrow are untouched. No panel/box around the
// radar: only an optional 1px outline just outside the radar's real edges,
// and one thin health | armour strip anchored to the radar's real rect.
//
// Health/armour: the CHud::RenderHealthBar / RenderArmorBar call sites inside
// CHud::DrawPlayerInfo are redirected (found at runtime by exact rel32
// target). GTA therefore still decides *when* the bars are shown; the block
// only replaces *where/how*. Entries hidden by omp-cef's HUD toggles (RET at
// the function entry) stay hidden. If the call sites are not found, nothing
// is redirected and vanilla bars remain.
//
// All functions run on the GTA thread.

namespace skr::radar_hud {

// Persisted in %LOCALAPPDATA%\SKR\hud.ini (re-read when the file changes).
// Distances are GTA HUD units (640x448 space), like the native radar.
struct Config {
    bool  frameEnabled  = true;   // [RadarFrame] Enabled  ignored since 22.5.1 (no outline)
    bool  vitalsEnabled = true;   // [Vitals] Enabled  (false = vanilla bars)
    bool  showNumbers   = false;  // [Vitals] ShowNumbers
    float offsetX       = 0.0f;   // [Vitals] OffsetX  -200..200, from the slot under the radar
    float offsetY       = 0.0f;   // [Vitals] OffsetY  -200..200
    float scale         = 1.0f;   // [Vitals] Scale     0.5..2.0 (strip thickness; width = radar)
    float spacing       = 2.0f;   // [Vitals] Spacing   0..20, radar → strip gap
    bool  debugGeometry = false;  // [Debug] Geometry  log measured radar/strip geometry (2 s)
    bool  debugOverlay  = false;  // [Debug] Overlay   draw measured radar/strip bounds
};

Config GetConfig();
// Clamps, applies immediately and writes hud.ini. Hook for a future
// settings page / drag edit mode.
bool SetConfig(const Config& config);

// Server override for Config::showNumbers (Native Runtime API, HudSet
// VitalsNumbers): 0 = hide, 1 = show, -1 = none (use hud.ini). Session-only:
// never written to hud.ini; native_api resets it to -1 on disconnect.
void SetServerVitalsNumbers(int value);

// Local player's health / max health / armour from the verified CPed fields
// (SEH-guarded). False when no player ped exists. Used by the ESC GAME page.
bool ReadPlayerVitals(float& health, float& maxHealth, float& armour);

// Square/custom mode: the SKR strip replaces vanilla bars. Round (default)
// mode: vanilla bars are left alone.  Called from game_state.cpp ApplyRadar.
void SetSquareMode(bool square);

// Called from game_state.cpp
void OnHudDrawBegin();                                   // before CHud::Draw
// Radar geometry captured inside CHud::DrawRadar (game_state.cpp
// HookedDrawRadar), while SA-MP's size values and SKR's scale are applied.
struct RadarGeometry {
    float srcWidth, srcHeight, srcTopFromBottom;   // .rdata HUD-unit values used this frame
    float formulaX, formulaY, formulaW, formulaH;  // old 40*sx formula — diagnostics only
    float x, y, w, h;   // measured: radar-space corners (-1,1)/(1,-1) through
                        // CRadar::TransformRadarPointToScreenSpace 0x583480 (screen px)
};
void OnRadarDrawn(const RadarGeometry& geometry);
void OnHudDrawEnd();                                     // after CHud::Draw

} // namespace skr::radar_hud
