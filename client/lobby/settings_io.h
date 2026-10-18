#pragma once
#include <string>
#include <vector>

// Persisted lobby settings — same INI format as apps/launcher-ui (parallel
// session). Section/key layout: [launcher] with nickname, gtaPath, resolution,
// windowed, intro, volume.

namespace skr::lobby {

struct Settings {
    std::string nickname  = "SKR_Player";
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
// Pure merge used by persistence; retains unrelated sections and comments.
std::string MergeSettingsIni(const std::string& text, const Settings& in);
bool NicknameFromJson(const std::string& json, std::string& out);
bool ConnectRequestFromJson(const std::string& json, std::string& address,
                            uint16_t& port, std::string& nickname);

// Mirrors the windowed/fullscreen toggle to sa-mp.cfg (key `fullscreen = 0|1`).
// Non-fatal: returns false but does not touch the lobby INI on failure.
bool MirrorFullscreenToSampCfg(bool windowed);

// Serialises settings for hydration. Incoming patches accept only resolution,
// windowed and volume; nickname has its own validated event.
std::string SettingsToJson(const Settings& s);
bool SettingsFromJson(const std::string& json, Settings& out);

// Servers the player added by address in the lobby ("Personal" tag).
// Stored in launcher.ini as a [servers] section, one `host:port=Name` per line.
struct PersonalServer {
    std::string address;   // canonical "host:port"
    std::string name;      // display name; may be empty
};

constexpr size_t kMaxPersonalServers = 50;
constexpr size_t kMaxServerNameBytes = 64;

// Validates and canonicalises "host:port" (host = IPv4 or DNS name, port
// 1..65535; host lowercased). Returns false for anything else (no IPv6).
bool NormalizeServerAddress(const std::string& in, std::string& out);

bool LoadPersonalServers(std::vector<PersonalServer>& out);
// Rewrites only the [servers] section; every other section is preserved.
bool SavePersonalServers(const std::vector<PersonalServer>& in);

// JSON array: [{"address":"host:port","name":"..."}, ...]
std::string PersonalServersToJson(const std::vector<PersonalServer>& list);
// Reads {"address":"...","name":"..."} sent by the lobby page.
bool PersonalServerFromJson(const std::string& json, PersonalServer& out);

} // namespace skr::lobby
