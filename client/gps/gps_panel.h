#pragma once
// ═══════════════════════════════════════════════════════════════════════
// GPS navigation panel above the minimap — native ImGui (API-003.2)
// ═══════════════════════════════════════════════════════════════════════
// Rendered by the single existing ImGui owner (native-pause renderer, inside
// its D3DSBT_ALL state block, at EndScene) while the pause menu is closed.
// No new hooks, no new ImGui context, no input capture: only draw-list
// commands on the foreground list. Layout/state: gps_panel_model.h.
// GTA thread only.
//
// Two widgets, chosen by gps_display_policy.h: the full panel while the local
// player drives, a distance-only box on the radar on foot / as passenger.

#include "gps_display_policy.h"

struct ImFont;
struct ImDrawList;

namespace skr::gps::panel {

struct View;
struct Layout;
struct CompactView;
struct CompactLayout;

// Game tick: current GTA zone names for the second row. Cheap to call; the
// caller rate-limits zone resolution (game_state.cpp).
void SetLocation(const char* area, const char* city);
bool WantsLocation();   // navigation state is not Idle

// Game tick: player position (world XY) and seat, read from CPed/CVehicle.
// The on-foot box shows the straight-line distance from here to the waypoint.
void SetPlayerContext(float x, float y, Seat seat);

// Display mode override for the GPS Pawn API (API-003.3). Default Auto.
void SetDisplayOverride(ModeOverride mode);
DisplayMode CurrentDisplayMode();

// EndScene: true when the panel has something to show AND GTA drew the
// minimap since the panel's previous frame (gps_renderer.h RadarFrame).
bool WantsRender();

// Inside an ImGui frame. Fonts may be null (ImGui default font is used).
// uiScale: ESC menu UI Scale; applied once, in ComputeLayout.
void DrawImGui(ImFont* heading, ImFont* body, float uiScale);

// The drawing itself (used by DrawImGui and by the offscreen preview):
// shadow, body, border, tone accent, label left / distance right on row 1,
// location left on row 2, text measured with the given fonts.
void DrawPanel(ImDrawList* dl, const View& view, const Layout& layout, ImFont* heading, ImFont* body);

// The on-foot distance box (layout from ComputeCompactLayout with the text
// width measured in `heading` at layout.font).
void DrawCompact(ImDrawList* dl, const CompactView& view, const CompactLayout& layout, ImFont* heading);

void Reset();

} // namespace skr::gps::panel
