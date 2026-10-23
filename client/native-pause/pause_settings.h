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

} // namespace skr::native_pause::settings
