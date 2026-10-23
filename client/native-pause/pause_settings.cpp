#include "native-pause/pause_settings.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace skr::native_pause::settings {
namespace {

// GTA SA 1.0 US addresses and layout. Provenance:
// - deps/omp-cef/src/client/core/game_sa/CMenuManager.{h,cpp}
// - deps/omp-cef/src/client/core/game_sa/CGamma.cpp
// - deps/omp-cef/src/client/core/game_sa/CCamera.cpp
// - deps/omp-cef/src/client/core/game_sa/CRenderer.cpp
// - deps/omp-cef/src/client/core/game_sa/CVehicle.cpp
// - gta-reversed MenuManager.cpp / AudioEngine.cpp / VideoMode.cpp
constexpr uintptr_t kMenuManager         = 0x00BA6748;
constexpr uintptr_t kBrightness          = kMenuManager + 0x3C;
constexpr uintptr_t kDrawDistance        = kMenuManager + 0x40;
constexpr uintptr_t kSubtitles           = kMenuManager + 0x44;
constexpr uintptr_t kWidescreen          = kMenuManager + 0x4B;
constexpr uintptr_t kFrameLimiter        = kMenuManager + 0x4C;
constexpr uintptr_t kRadioAutoSelect     = kMenuManager + 0x4D;
constexpr uintptr_t kSfxVolume           = kMenuManager + 0x4F;
constexpr uintptr_t kRadioVolume         = kMenuManager + 0x50;
constexpr uintptr_t kRadioEq             = kMenuManager + 0x51;
constexpr uintptr_t kHud                 = kMenuManager + 0x21;
constexpr uintptr_t kMipMapping          = kMenuManager + 0xC4;
constexpr uintptr_t kAppliedAntiAliasing = kMenuManager + 0xC8;
constexpr uintptr_t kAntiAliasing        = kMenuManager + 0xCC;
constexpr uintptr_t kAppliedVideoMode    = kMenuManager + 0xD4;
constexpr uintptr_t kPreferredVideoMode  = kMenuManager + 0xD8;

constexpr uintptr_t kInvertMouseY        = 0x00BA6745;
constexpr uintptr_t kMouseSensitivity    = 0x00B6EC1C;
constexpr uintptr_t kMouseSteering       = 0x00C1CC02;
constexpr uintptr_t kMouseFlying         = 0x00C1CC03;
constexpr uintptr_t kRendererLodScale    = 0x008CD800;

constexpr uintptr_t kGammaObject         = 0x00C92134;
constexpr uintptr_t kSetGamma            = 0x00747200;
constexpr uintptr_t kAudioEngine         = 0x00B6BC90;
constexpr uintptr_t kSetMusicVolume      = 0x00506DE0;
constexpr uintptr_t kSetEffectsVolume    = 0x00506E10;
constexpr uintptr_t kSetRadioAutoRetune  = 0x00506F80;
constexpr uintptr_t kSetBassEnhance      = 0x00506F90;
constexpr uintptr_t kSaveSettings        = 0x0057C660;

constexpr uintptr_t kGetNumVideoModes    = 0x007F2CC0;
constexpr uintptr_t kGetVideoModeInfo    = 0x007F2CF0;
constexpr uintptr_t kGetCurrentVideoMode = 0x007F2D20;
constexpr uintptr_t kSetVideoMode        = 0x00745C70;
constexpr uintptr_t kSetMipMapping       = 0x007F3530;
constexpr uintptr_t kGetMaxMultiSampling = 0x007F84E0;
constexpr uintptr_t kSetMultiSampling    = 0x007F8A90;

constexpr UINT kApplyVideoModeMessage = WM_APP + 0x622;
constexpr UINT kApplyAntiAliasingMessage = WM_APP + 0x623;
constexpr int kMaxVideoModes = 256;
constexpr int kVideoModeExclusive = 0x1;

struct RwVideoModeInfo {
    int width;
    int height;
    int depth;
    int flags;
    int refreshRate;
    int format;
};

using SaveSettingsFn = void(__thiscall*)(void*);
using SetGammaFn = void(__thiscall*)(void*, float, char);
using AudioValueFn = void(__thiscall*)(void*, int);
using AudioBoolFn = void(__thiscall*)(void*, bool);
using GetNumVideoModesFn = int(__cdecl*)();
using GetVideoModeInfoFn = RwVideoModeInfo*(__cdecl*)(RwVideoModeInfo*, int);
using GetCurrentVideoModeFn = int(__cdecl*)();
using SetVideoModeFn = void(__cdecl*)(int);
using SetMipMappingFn = void(__cdecl*)(bool);
using GetMaxMultiSamplingFn = int(__cdecl*)();
using SetMultiSamplingFn = void(__cdecl*)(int);

HWND g_window = nullptr;
float g_uiScale = 1.0f;        // SKR UI scale multiplier (0.75 – 1.50)
bool  g_uiScaleLoaded = false;

// ── SKR UI scale persistence (file-based, not GTA engine) ──────────
static char g_uiScalePath[MAX_PATH] = {};

static const char* GetUiScalePath() {
    if (g_uiScalePath[0]) return g_uiScalePath;
    DWORD n = GetModuleFileNameA(nullptr, g_uiScalePath, MAX_PATH);
    if (!n || n >= MAX_PATH) return nullptr;
    char* slash = strrchr(g_uiScalePath, '\\');
    if (!slash) return nullptr;
    snprintf(slash + 1, MAX_PATH - (slash + 1 - g_uiScalePath), "SKR\\ui_scale.cfg");
    return g_uiScalePath;
}

static void LoadUiScale() {
    if (g_uiScaleLoaded) return;
    g_uiScaleLoaded = true;
    g_uiScale = 1.0f;
    const char* path = GetUiScalePath();
    if (!path) return;
    FILE* f = fopen(path, "r");
    if (!f) return;
    float val = 1.0f;
    if (fscanf(f, "%f", &val) == 1)
        g_uiScale = std::clamp(val, 0.75f, 1.50f);
    fclose(f);
}

static bool SaveUiScale() {
    const char* path = GetUiScalePath();
    if (!path) return false;
    FILE* f = fopen(path, "w");
    if (!f) return false;
    fprintf(f, "%.2f\n", g_uiScale);
    fclose(f);
    return true;
}

int g_selectedVideoMode = -1;
int g_requestedVideoMode = -1;
bool g_videoRequestPending = false;
int g_selectedAntiAliasing = 1;
int g_requestedAntiAliasing = 1;
bool g_antiAliasingRequestPending = false;

template <typename T>
T& Ref(uintptr_t address) {
    return *reinterpret_cast<T*>(address);
}

bool Save() {
    __try {
        reinterpret_cast<SaveSettingsFn>(kSaveSettings)(reinterpret_cast<void*>(kMenuManager));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadVideoMode(int index, VideoMode& out) {
    __try {
        const int count = reinterpret_cast<GetNumVideoModesFn>(kGetNumVideoModes)();
        if (index < 0 || index >= count) return false;
        RwVideoModeInfo info{};
        if (!reinterpret_cast<GetVideoModeInfoFn>(kGetVideoModeInfo)(&info, index))
            return false;
        out = {index, info.width, info.height, info.depth, info.refreshRate,
               (info.flags & kVideoModeExclusive) != 0};
        return info.width > 0 && info.height > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeSetVideoMode(int mode) {
    __try {
        reinterpret_cast<SetVideoModeFn>(kSetVideoMode)(mode);
        return reinterpret_cast<GetCurrentVideoModeFn>(kGetCurrentVideoMode)() == mode;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ApplyVideoMode(int target) {
    VideoMode targetInfo{};
    if (!ReadVideoMode(target, targetInfo)) return false;

    int previous = -1;
    __try {
        const auto getCurrent = reinterpret_cast<GetCurrentVideoModeFn>(kGetCurrentVideoMode);
        previous = getCurrent();
        if (previous == target) {
            Ref<int>(kAppliedVideoMode) = target;
            Ref<int>(kPreferredVideoMode) = target;
            return Save();
        }

        if (!SafeSetVideoMode(target)) {
            if (previous >= 0) SafeSetVideoMode(previous);
            return false;
        }

        Ref<int>(kAppliedVideoMode) = target;
        Ref<int>(kPreferredVideoMode) = target;
        if (!Save()) {
            if (previous >= 0) SafeSetVideoMode(previous);
            Ref<int>(kAppliedVideoMode) = previous;
            Ref<int>(kPreferredVideoMode) = previous;
            return false;
        }
        g_selectedVideoMode = target;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (previous >= 0) SafeSetVideoMode(previous);
        return false;
    }
}

int MaxAntiAliasing() {
    __try {
        const int supported = reinterpret_cast<GetMaxMultiSamplingFn>(kGetMaxMultiSampling)();
        return std::clamp(supported, 1, 4);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}

bool ApplyAntiAliasing(int target) {
    const int maximum = MaxAntiAliasing();
    if (target < 1 || target > maximum) return false;

    int previousApplied = 1;
    int previousPreferred = 1;
    int videoMode = -1;
    bool stateChanged = false;
    __try {
        previousApplied = Ref<int>(kAppliedAntiAliasing);
        previousPreferred = Ref<int>(kAntiAliasing);
        videoMode = Ref<int>(kAppliedVideoMode);
        Ref<int>(kAppliedAntiAliasing) = target;
        Ref<int>(kAntiAliasing) = target;
        stateChanged = true;
        reinterpret_cast<SetMultiSamplingFn>(kSetMultiSampling)(target);
        if (!SafeSetVideoMode(videoMode) || !Save()) {
            Ref<int>(kAppliedAntiAliasing) = previousApplied;
            Ref<int>(kAntiAliasing) = previousPreferred;
            reinterpret_cast<SetMultiSamplingFn>(kSetMultiSampling)(previousApplied);
            SafeSetVideoMode(videoMode);
            return false;
        }
        g_selectedAntiAliasing = target;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (stateChanged) {
            __try {
                Ref<int>(kAppliedAntiAliasing) = previousApplied;
                Ref<int>(kAntiAliasing) = previousPreferred;
                reinterpret_cast<SetMultiSamplingFn>(kSetMultiSampling)(previousApplied);
                if (videoMode >= 0) SafeSetVideoMode(videoMode);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
        return false;
    }
}

template <typename T>
bool StoreAndSave(uintptr_t address, T value) {
    __try {
        Ref<T>(address) = value;
        return Save();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

void Initialize(HWND window) {
    g_window = window;
    Refresh();
}

void Shutdown() {
    g_window = nullptr;
    g_selectedVideoMode = -1;
    g_requestedVideoMode = -1;
    g_videoRequestPending = false;
    g_selectedAntiAliasing = 1;
    g_requestedAntiAliasing = 1;
    g_antiAliasingRequestPending = false;
}

void Refresh() {
    __try {
        g_selectedVideoMode = reinterpret_cast<GetCurrentVideoModeFn>(kGetCurrentVideoMode)();
        g_selectedAntiAliasing = std::clamp(Ref<int>(kAntiAliasing), 1, MaxAntiAliasing());
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_selectedVideoMode = -1;
    }
}

bool Read(Snapshot& out) {
    __try {
        out.brightness = Ref<int>(kBrightness);
        out.drawDistance = Ref<float>(kDrawDistance);
        out.subtitles = Ref<bool>(kSubtitles);
        out.widescreen = Ref<bool>(kWidescreen);
        out.frameLimiter = Ref<bool>(kFrameLimiter);
        out.hud = Ref<bool>(kHud);
        out.mipMapping = Ref<bool>(kMipMapping);
        out.antiAliasing = Ref<int>(kAntiAliasing);
        out.sfxVolume = static_cast<unsigned char>(Ref<char>(kSfxVolume));
        out.radioVolume = static_cast<unsigned char>(Ref<char>(kRadioVolume));
        out.radioEq = Ref<bool>(kRadioEq);
        out.radioAutoSelect = Ref<bool>(kRadioAutoSelect);
        out.mouseSensitivity = Ref<float>(kMouseSensitivity);
        out.invertMouseY = Ref<bool>(kInvertMouseY);
        out.mouseSteering = Ref<bool>(kMouseSteering);
        out.mouseFlying = Ref<bool>(kMouseFlying);
        out.currentVideoMode = reinterpret_cast<GetCurrentVideoModeFn>(kGetCurrentVideoMode)();
        LoadUiScale();
        out.uiScale = g_uiScale;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::memset(&out, 0, sizeof(out));
        return false;
    }
}

int GetVideoModes(VideoMode* out, int capacity) {
    if (!out || capacity <= 0) return 0;
    int written = 0;
    __try {
        const int reported = reinterpret_cast<GetNumVideoModesFn>(kGetNumVideoModes)();
        const int count = reported < kMaxVideoModes ? reported : kMaxVideoModes;
        for (int i = 0; i < count && written < capacity; ++i) {
            VideoMode mode{};
            if (ReadVideoMode(i, mode) && mode.width >= 640 && mode.height >= 480)
                out[written++] = mode;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return written;
}

bool GetSelectedVideoMode(VideoMode& out) {
    if (g_selectedVideoMode < 0) Refresh();
    return ReadVideoMode(g_selectedVideoMode, out);
}

void CycleSelectedVideoMode(int direction) {
    VideoMode modes[kMaxVideoModes]{};
    const int count = GetVideoModes(modes, kMaxVideoModes);
    if (!count || direction == 0) return;

    int position = 0;
    for (int i = 0; i < count; ++i) {
        if (modes[i].index == g_selectedVideoMode) {
            position = i;
            break;
        }
    }
    position = (position + (direction > 0 ? 1 : count - 1)) % count;
    g_selectedVideoMode = modes[position].index;
}

bool RequestSelectedVideoMode() {
    if (!g_window || g_selectedVideoMode < 0 || g_videoRequestPending)
        return false;
    g_requestedVideoMode = g_selectedVideoMode;
    g_videoRequestPending = PostMessageW(g_window, kApplyVideoModeMessage, 0, 0) != FALSE;
    return g_videoRequestPending;
}

int GetSelectedAntiAliasing() {
    return g_selectedAntiAliasing;
}

void CycleSelectedAntiAliasing(int direction) {
    if (!direction) return;
    const int maximum = MaxAntiAliasing();
    g_selectedAntiAliasing += direction > 0 ? 1 : -1;
    if (g_selectedAntiAliasing > maximum) g_selectedAntiAliasing = 1;
    if (g_selectedAntiAliasing < 1) g_selectedAntiAliasing = maximum;
}

bool RequestSelectedAntiAliasing() {
    if (!g_window || g_antiAliasingRequestPending) return false;
    g_requestedAntiAliasing = g_selectedAntiAliasing;
    g_antiAliasingRequestPending =
        PostMessageW(g_window, kApplyAntiAliasingMessage, 0, 0) != FALSE;
    return g_antiAliasingRequestPending;
}

bool ProcessMessage(UINT msg, WPARAM, LPARAM) {
    if (msg == kApplyVideoModeMessage) {
        const int target = g_requestedVideoMode;
        g_videoRequestPending = false;
        g_requestedVideoMode = -1;
        if (!ApplyVideoMode(target)) Refresh();
        return true;
    }
    if (msg == kApplyAntiAliasingMessage) {
        const int target = g_requestedAntiAliasing;
        g_antiAliasingRequestPending = false;
        if (!ApplyAntiAliasing(target)) Refresh();
        return true;
    }
    return false;
}

bool SetBrightness(int value, bool persist) {
    value = std::clamp(value, 0, 384);
    __try {
        Ref<int>(kBrightness) = value;
        reinterpret_cast<SetGammaFn>(kSetGamma)(reinterpret_cast<void*>(kGammaObject),
                                                static_cast<float>(value), TRUE);
        return !persist || Save();
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool SetDrawDistance(float value, bool persist) {
    value = std::clamp(value, 0.925f, 1.8f);
    __try {
        Ref<float>(kDrawDistance) = value;
        Ref<float>(kRendererLodScale) = value;
        return !persist || Save();
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool SetSfxVolume(int value, bool persist) {
    value = std::clamp(value, 0, 64);
    __try {
        Ref<char>(kSfxVolume) = static_cast<char>(value);
        reinterpret_cast<AudioValueFn>(kSetEffectsVolume)(reinterpret_cast<void*>(kAudioEngine), value);
        return !persist || Save();
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool SetRadioVolume(int value, bool persist) {
    value = std::clamp(value, 0, 64);
    __try {
        Ref<char>(kRadioVolume) = static_cast<char>(value);
        reinterpret_cast<AudioValueFn>(kSetMusicVolume)(reinterpret_cast<void*>(kAudioEngine), value);
        return !persist || Save();
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool SetMouseSensitivity(float value, bool persist) {
    value = std::clamp(value, 0.0003125f, 0.005f);
    __try {
        Ref<float>(kMouseSensitivity) = value;
        return !persist || Save();
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool SetSubtitles(bool value) { return StoreAndSave(kSubtitles, value); }
bool SetWidescreen(bool value) { return StoreAndSave(kWidescreen, value); }
bool SetFrameLimiter(bool value) { return StoreAndSave(kFrameLimiter, value); }
bool SetHud(bool value) { return StoreAndSave(kHud, value); }

bool SetMipMapping(bool value) {
    __try {
        Ref<bool>(kMipMapping) = value;
        reinterpret_cast<SetMipMappingFn>(kSetMipMapping)(value);
        return Save();
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool SetRadioEq(bool value) {
    __try {
        Ref<bool>(kRadioEq) = value;
        reinterpret_cast<AudioBoolFn>(kSetBassEnhance)(reinterpret_cast<void*>(kAudioEngine), value);
        return Save();
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool SetRadioAutoSelect(bool value) {
    __try {
        Ref<bool>(kRadioAutoSelect) = value;
        reinterpret_cast<AudioBoolFn>(kSetRadioAutoRetune)(reinterpret_cast<void*>(kAudioEngine), value);
        return Save();
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool SetInvertMouseY(bool value) { return StoreAndSave(kInvertMouseY, value); }
bool SetMouseSteering(bool value) { return StoreAndSave(kMouseSteering, value); }
bool SetMouseFlying(bool value) { return StoreAndSave(kMouseFlying, value); }

float GetUiScale() {
    LoadUiScale();
    return g_uiScale;
}

bool SetUiScale(float value) {
    g_uiScale = std::clamp(value, 0.75f, 1.50f);
    g_uiScaleLoaded = true;
    return SaveUiScale();
}

} // namespace skr::native_pause::settings
