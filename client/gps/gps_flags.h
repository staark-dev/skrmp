#pragma once
// ═══════════════════════════════════════════════════════════════════════
// GPS diagnostic feature switches (API-003.0.3)
// ═══════════════════════════════════════════════════════════════════════
// %LOCALAPPDATA%\SKR\gps_debug.ini, section [GPS]:
//   Core=0       gps::Tick (waypoint scan, pathfinding, navigation)
//   Renderer=0   gps::RenderRoute (minimap route line)
//   Waypoints=0  ESC MAP right-click / CLEAR WAYPOINT / marker / route
//   AdaptiveZoom=0  navigation radar range (gps_zoom.h)
// Missing file or key = disabled. The file is re-read (at most once per
// second, only from CoreEnabled() on the game tick — never on the render
// path) when its timestamp changes, so stages can be switched in-game.
// All functions run on the GTA thread.

#include <optional>

namespace skr::gps::flags {

struct Flags {
    bool core      = false;
    bool renderer  = false;
    bool waypoints = false;
    bool zoom      = false;
};

// What must be torn down when switching from `before` to `after`.
struct Transition {
    bool resetCore     = false;   // gps::Reset() + ResetRenderer()
    bool resetRenderer = false;   // ResetRenderer()
};

inline Transition Diff(const Flags& before, const Flags& after) {
    Transition t;
    t.resetCore     = before.core && !after.core;
    t.resetRenderer = t.resetCore || (before.renderer && !after.renderer);
    return t;
}

bool CoreEnabled();        // polls gps_debug.ini (game tick)
bool CoreEnabledCached();  // render path (ESC MAP): never touches the file system
bool RendererEnabled();    // cached
bool WaypointsEnabled();   // cached
bool ZoomEnabled();        // cached

// Tests: fixed switches, bypassing gps_debug.ini. std::nullopt restores the file.
void SetForTesting(std::optional<Flags> fixed);

} // namespace skr::gps::flags

namespace skr::gps {
// Appends a line to <gta>\SKR\gps.log.
void Log(const char* fmt, ...);
}
