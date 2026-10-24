#pragma once
// ═══════════════════════════════════════════════════════════════════════
// SKR Enhanced client settings (ESC → SETTINGS → ENHANCED)
// ═══════════════════════════════════════════════════════════════════════
// Stored separately from GTA's gta_sa.set, next to the existing client
// preference files in %LOCALAPPDATA%\SKR\:
//   client.ini — this module ([Interface] UiScale, Animations;
//                [Client] DebugLog, ShowVersion)
//   fps.ini    — SKR FPS engine (shared with the launcher's FPS panel)
//   hud.ini    — native radar outline / health & armour block (radar_hud)
// Only options with a real implementation are exposed.

namespace skr::native_pause::enhanced {

struct Prefs {
    float uiScale     = 1.0f;   // 0.75 – 1.50, ESC menu layout scale
    bool  animations  = true;   // ESC menu fade-in
    bool  debugLog    = true;   // writes SKR\native_pause.log
    bool  showVersion = false;  // client version in the ESC footer
};

const Prefs& Get();              // loads client.ini on first use
bool Set(const Prefs& prefs);    // clamps, applies, writes client.ini

// FPS engine preferences (fps.ini, applied live).
bool GetFpsCounter();
unsigned GetFpsCap();            // 0 = no SKR cap
bool SetFps(bool showCounter, unsigned cap);
const unsigned* FpsCapChoices(int& count);

// Resets client.ini, fps.ini and hud.ini to their defaults.
bool ResetAll();

// Fast check for the native-pause loggers.
bool DebugLogEnabled();

} // namespace skr::native_pause::enhanced
