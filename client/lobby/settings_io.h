#pragma once
#include <string>

// Persisted lobby settings — same INI format as apps/launcher-ui (parallel
// session). Section/key layout: [launcher] with nickname, gtaPath, resolution,
// windowed, intro, volume.

namespace skr::lobby {

struct Settings {
    std::string nickname  = "Jucător";
    std::string gtaPath;              // informative only; set by the launcher
    std::string resolution = "auto";  // "1920x1080" | "1600x900" | ... | "auto"
    bool        windowed  = false;
    bool        intro     = true;
    int         volume    = 80;       // 0..100
};

// Resolves "%APPDATA%\SKR\launcher.ini". Returns empty on failure.
std::wstring LauncherIniPath();

// Resolves "<Documents>\GTA San Andreas User Files\SAMP\sa-mp.cfg".
std::wstring SampCfgPath();

bool LoadSettings(Settings& out);
bool SaveSettings(const Settings& in);

// Mirrors the windowed/fullscreen toggle to sa-mp.cfg (key `fullscreen = 0|1`).
// Non-fatal: returns false but does not touch the lobby INI on failure.
bool MirrorFullscreenToSampCfg(bool windowed);

// Serialises/parses settings as JSON (compact). Used by the CEF event bridge.
std::string SettingsToJson(const Settings& s);
bool SettingsFromJson(const std::string& json, Settings& out);

} // namespace skr::lobby
