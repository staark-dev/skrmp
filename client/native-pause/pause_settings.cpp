#include "native-pause/pause_settings.h"
#include "native-pause/pause_enhanced.h"

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

// Milestone B — CMenuManager fields (deps/omp-cef/src/client/core/game_sa/
// CMenuManager.h; consistent with the verified offsets above):
constexpr uintptr_t kRadarMode           = kMenuManager + 0x24;   // int
constexpr uintptr_t kLanguage            = kMenuManager + 0x84;   // char
constexpr uintptr_t kPreviousLanguage    = kMenuManager + 0x85;   // char
constexpr uintptr_t kLanguageChanged     = kMenuManager + 0x8C;   // bool
constexpr uintptr_t kUserTrackMode       = kMenuManager + 0xB0;   // char m_nRadioMode
constexpr uintptr_t kTracksAutoScan      = kMenuManager + 0xC5;   // bool
constexpr uintptr_t kSavePhotos          = kMenuManager + 0xE8;   // bool
constexpr uintptr_t kSetDefaultPrefs     = 0x00573AE0;  // char CMenuManager::SetDefaultPreferences(eMenuPage)
constexpr uintptr_t kInitChangedLanguage = 0x00573260;  // char CMenuManager::InitialiseChangedLanguageSettings(char)
// Fx_c (Fx_c.cpp): g_fx 0xA9AE00, SetFxQuality 0x49EA40, GetFxQuality 0x49EA50 (thiscall)
constexpr uintptr_t kFx                  = 0x00A9AE00;
constexpr uintptr_t kFxSetQuality        = 0x0049EA40;
constexpr uintptr_t kFxGetQuality        = 0x0049EA50;

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
constexpr UINT kApplyLanguageMessage = WM_APP + 0x624;
constexpr UINT kApplyDefaultsMessage = WM_APP + 0x625;   // wParam = eMenuPage
constexpr DWORD kVideoConfirmMs = 15000;
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
using MenuPageFn = char(__thiscall*)(void*, int);
using MenuCharFn = char(__thiscall*)(void*, char);
using FxSetQualityFn = void(__thiscall*)(void*, unsigned int);
using FxGetQualityFn = unsigned int(__thiscall*)(void*);

HWND g_window = nullptr;
// SKR UI scale moved to the Enhanced store (pause_enhanced.cpp, client.ini);
// the legacy SKR\ui_scale.cfg is migrated there on first run.

int g_selectedVideoMode = -1;
int g_requestedVideoMode = -1;
bool g_videoRequestPending = false;
int g_selectedAntiAliasing = 1;
int g_requestedAntiAliasing = 1;
bool g_antiAliasingRequestPending = false;

// Milestone B state
int   g_selectedLanguage = -1;
int   g_requestedLanguage = -1;
bool  g_languageRequestPending = false;
bool  g_defaultsRequestPending = false;
bool  g_videoConfirmPending = false;
int   g_videoConfirmPrevious = -1;
DWORD g_videoConfirmDeadline = 0;
bool  g_videoRevertInProgress = false;

constexpr const char* kLanguageFiles[] = { "american", "french", "german", "italian", "spanish" };
constexpr const char* kLanguageNames[] = { "English", "French", "German", "Italian", "Spanish" };

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
        if (!g_videoRevertInProgress && previous >= 0) {
            // Keep-or-revert window (see VideoConfirmPending / Tick).
            g_videoConfirmPending = true;
            g_videoConfirmPrevious = previous;
            g_videoConfirmDeadline = GetTickCount() + kVideoConfirmMs;
        }
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

// Re-applies the runtime side effects of values written by
// SetDefaultPreferences, so gamma, LOD, mip mapping and audio match at once.
void ApplyRuntimeValues() {
    __try {
        reinterpret_cast<SetGammaFn>(kSetGamma)(reinterpret_cast<void*>(kGammaObject),
                                                static_cast<float>(Ref<int>(kBrightness)), TRUE);
        Ref<float>(kRendererLodScale) = Ref<float>(kDrawDistance);
        reinterpret_cast<SetMipMappingFn>(kSetMipMapping)(Ref<bool>(kMipMapping));
        void* audio = reinterpret_cast<void*>(kAudioEngine);
        reinterpret_cast<AudioValueFn>(kSetEffectsVolume)(audio,
            static_cast<unsigned char>(Ref<char>(kSfxVolume)));
        reinterpret_cast<AudioValueFn>(kSetMusicVolume)(audio,
            static_cast<unsigned char>(Ref<char>(kRadioVolume)));
        reinterpret_cast<AudioBoolFn>(kSetBassEnhance)(audio, Ref<bool>(kRadioEq));
        reinterpret_cast<AudioBoolFn>(kSetRadioAutoRetune)(audio, Ref<bool>(kRadioAutoSelect));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool ApplyDefaults(int page) {
    __try {
        reinterpret_cast<MenuPageFn>(kSetDefaultPrefs)(reinterpret_cast<void*>(kMenuManager), page);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    ApplyRuntimeValues();
    return Save();
}

bool ApplyLanguage(int target) {
    if (target < 0 || target >= kLanguageCount) return false;
    __try {
        const char current = Ref<char>(kLanguage);
        if (current == static_cast<char>(target)) return true;
        Ref<char>(kPreviousLanguage) = current;
        Ref<char>(kLanguage) = static_cast<char>(target);
        Ref<bool>(kLanguageChanged) = true;
        // Same sequence as the vanilla language page: reload text, no control re-init.
        reinterpret_cast<MenuCharFn>(kInitChangedLanguage)(reinterpret_cast<void*>(kMenuManager), 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return Save();
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
    g_selectedLanguage = -1;
    g_languageRequestPending = false;
    g_defaultsRequestPending = false;
    g_videoConfirmPending = false;
    g_videoRevertInProgress = false;
}

void Refresh() {
    __try {
        g_selectedVideoMode = reinterpret_cast<GetCurrentVideoModeFn>(kGetCurrentVideoMode)();
        g_selectedAntiAliasing = std::clamp(Ref<int>(kAntiAliasing), 1, MaxAntiAliasing());
        g_selectedLanguage = static_cast<unsigned char>(Ref<char>(kLanguage));
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
        out.radarMode = Ref<int>(kRadarMode);
        out.savePhotos = Ref<bool>(kSavePhotos);
        out.fxQuality = static_cast<int>(
            reinterpret_cast<FxGetQualityFn>(kFxGetQuality)(reinterpret_cast<void*>(kFx)));
        out.userTrackMode = static_cast<unsigned char>(Ref<char>(kUserTrackMode));
        out.tracksAutoScan = Ref<bool>(kTracksAutoScan);
        out.language = static_cast<unsigned char>(Ref<char>(kLanguage));
        out.uiScale = enhanced::Get().uiScale;
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

bool ProcessMessage(UINT msg, WPARAM wParam, LPARAM) {
    if (msg == kApplyVideoModeMessage) {
        const int target = g_requestedVideoMode;
        g_videoRequestPending = false;
        g_requestedVideoMode = -1;
        if (!ApplyVideoMode(target)) Refresh();
        g_videoRevertInProgress = false;
        return true;
    }
    if (msg == kApplyLanguageMessage) {
        g_languageRequestPending = false;
        if (!ApplyLanguage(g_requestedLanguage)) Refresh();
        return true;
    }
    if (msg == kApplyDefaultsMessage) {
        g_defaultsRequestPending = false;
        ApplyDefaults(static_cast<int>(wParam));
        Refresh();
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

// ── Milestone B ─────────────────────────────────────────────────────
bool SetRadarMode(int value) { return StoreAndSave(kRadarMode, std::clamp(value, 0, 2)); }
bool SetSavePhotos(bool value) { return StoreAndSave(kSavePhotos, value); }
bool SetUserTrackMode(int value) {
    return StoreAndSave(kUserTrackMode, static_cast<char>(std::clamp(value, 0, 2)));
}
bool SetTracksAutoScan(bool value) { return StoreAndSave(kTracksAutoScan, value); }

bool SetFxQuality(int value) {
    value = std::clamp(value, 0, 3);
    __try {
        reinterpret_cast<FxSetQualityFn>(kFxSetQuality)(reinterpret_cast<void*>(kFx),
                                                        static_cast<unsigned int>(value));
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return Save();
}

const char* LanguageName(int language) {
    return (language >= 0 && language < kLanguageCount) ? kLanguageNames[language] : "Unknown";
}

bool IsLanguageInstalled(int language) {
    if (language < 0 || language >= kLanguageCount) return false;
    char path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* slash = (n && n < MAX_PATH) ? strrchr(path, '\\') : nullptr;
    if (!slash) return false;
    snprintf(slash + 1, MAX_PATH - (slash + 1 - path), "text\\%s.gxt", kLanguageFiles[language]);
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

int GetSelectedLanguage() {
    if (g_selectedLanguage < 0) Refresh();
    return g_selectedLanguage;
}

void CycleSelectedLanguage(int direction) {
    if (!direction) return;
    int next = GetSelectedLanguage();
    for (int i = 0; i < kLanguageCount; ++i) {
        next = (next + (direction > 0 ? 1 : kLanguageCount - 1)) % kLanguageCount;
        if (IsLanguageInstalled(next)) { g_selectedLanguage = next; return; }
    }
}

bool RequestSelectedLanguage() {
    if (!g_window || g_languageRequestPending || !IsLanguageInstalled(g_selectedLanguage))
        return false;
    g_requestedLanguage = g_selectedLanguage;
    g_languageRequestPending =
        PostMessageW(g_window, kApplyLanguageMessage, 0, 0) != FALSE;
    return g_languageRequestPending;
}

bool RequestDefaults(DefaultsGroup group) {
    if (!g_window || g_defaultsRequestPending) return false;
    g_defaultsRequestPending = PostMessageW(g_window, kApplyDefaultsMessage,
                                            static_cast<WPARAM>(group), 0) != FALSE;
    return g_defaultsRequestPending;
}

bool SaveNativeSettings() {
    return Save();
}

bool ApplyDefaultsNow(DefaultsGroup group) {
    return ApplyDefaults(static_cast<int>(group));
}

bool VideoConfirmPending(int& secondsLeft) {
    if (!g_videoConfirmPending) return false;
    const DWORD now = GetTickCount();
    const DWORD left = (static_cast<int32_t>(g_videoConfirmDeadline - now) > 0)
                           ? g_videoConfirmDeadline - now : 0;
    secondsLeft = static_cast<int>((left + 999) / 1000);
    return true;
}

void ConfirmVideoMode() {
    g_videoConfirmPending = false;
}

bool RevertVideoMode() {
    if (!g_videoConfirmPending) return false;
    g_videoConfirmPending = false;
    if (g_videoConfirmPrevious < 0) return false;
    g_videoRevertInProgress = true;          // no new confirmation for the revert
    g_selectedVideoMode = g_videoConfirmPrevious;
    if (!RequestSelectedVideoMode()) {
        g_videoRevertInProgress = false;
        return false;
    }
    return true;
}

void Tick() {
    if (g_videoConfirmPending &&
        static_cast<int32_t>(GetTickCount() - g_videoConfirmDeadline) >= 0)
        RevertVideoMode();
}

float GetUiScale() {
    return enhanced::Get().uiScale;
}

bool SetUiScale(float value) {
    enhanced::Prefs prefs = enhanced::Get();
    prefs.uiScale = value;
    return enhanced::Set(prefs);
}

} // namespace skr::native_pause::settings
