#pragma once

#include <windows.h>

namespace skr::native_pause::settings {

struct Snapshot {
    int brightness;
    float drawDistance;
    bool subtitles;
    bool widescreen;
    bool frameLimiter;
    bool hud;
    bool mipMapping;
    int antiAliasing;

    int sfxVolume;
    int radioVolume;
    bool radioEq;
    bool radioAutoSelect;

    float mouseSensitivity;
    bool invertMouseY;
    bool mouseSteering;
    bool mouseFlying;

    int currentVideoMode;

    // Milestone B — original GTA SA PC options
    int  radarMode;        // 0 map & blips, 1 blips only, 2 off
    bool savePhotos;       // store gallery photos
    int  fxQuality;        // 0 low, 1 medium, 2 high, 3 very high
    int  userTrackMode;    // 0 radio, 1 random, 2 sequential
    bool tracksAutoScan;
    int  language;         // 0 English, 1 French, 2 German, 3 Italian, 4 Spanish

    float uiScale;  // SKR menu UI scale multiplier (0.75 – 1.50)
};

struct VideoMode {
    int index;
    int width;
    int height;
    int depth;
    int refreshRate;
    bool exclusive;
};

void Initialize(HWND window);
void Shutdown();
void Refresh();

bool Read(Snapshot& out);
int GetVideoModes(VideoMode* out, int capacity);
bool GetSelectedVideoMode(VideoMode& out);
void CycleSelectedVideoMode(int direction);
bool RequestSelectedVideoMode();
int GetSelectedAntiAliasing();
void CycleSelectedAntiAliasing(int direction);
bool RequestSelectedAntiAliasing();
bool ProcessMessage(UINT msg, WPARAM wParam, LPARAM lParam);

bool SetBrightness(int value, bool persist);
bool SetDrawDistance(float value, bool persist);
bool SetSfxVolume(int value, bool persist);
bool SetRadioVolume(int value, bool persist);
bool SetMouseSensitivity(float value, bool persist);
bool SetSubtitles(bool value);
bool SetWidescreen(bool value);
bool SetFrameLimiter(bool value);
bool SetHud(bool value);
bool SetMipMapping(bool value);
bool SetRadioEq(bool value);
bool SetRadioAutoSelect(bool value);
bool SetInvertMouseY(bool value);
bool SetMouseSteering(bool value);
bool SetMouseFlying(bool value);

float GetUiScale();
bool  SetUiScale(float value);

// ── Milestone B ─────────────────────────────────────────────────────
bool SetRadarMode(int value);
bool SetSavePhotos(bool value);
bool SetFxQuality(int value);
bool SetUserTrackMode(int value);
bool SetTracksAutoScan(bool value);

// Language: only languages whose text\<name>.gxt is installed are offered.
constexpr int kLanguageCount = 5;
const char* LanguageName(int language);
bool IsLanguageInstalled(int language);
int  GetSelectedLanguage();
void CycleSelectedLanguage(int direction);
bool RequestSelectedLanguage();          // applied in the WndProc (text reload)

// GTA's own CMenuManager::SetDefaultPreferences for one settings group,
// applied in the WndProc, then saved. Display also covers graphics options.
// The Controls page resets preferences only; bindings are restored through
// pause_controls and CControllerConfigManager::ReinitControls.
enum class DefaultsGroup : int { Display = 4, Audio = 3, Controls = 36 };
bool RequestDefaults(DefaultsGroup group);

// Shared persistence entry points for the native controls transaction.
// These remain narrow wrappers around CMenuManager; callers must perform
// their own readback and rollback.
bool SaveNativeSettings();
bool ApplyDefaultsNow(DefaultsGroup group);

// Resolution changes must be confirmed; otherwise the previous mode is
// restored after a timeout. Tick() runs every frame (actions::Tick).
bool VideoConfirmPending(int& secondsLeft);
void ConfirmVideoMode();
bool RevertVideoMode();
void Tick();

} // namespace skr::native_pause::settings
