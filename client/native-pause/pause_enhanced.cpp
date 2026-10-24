#include "native-pause/pause_enhanced.h"

#include "performance/fps_settings.h"
#include "render-d3d9/fps_engine.h"
#include "game-state/radar_hud.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>

namespace skr::native_pause::enhanced {
namespace {

Prefs g_prefs;
bool  g_loaded = false;

// Same location scheme as shared/performance/fps_settings.cpp.
std::wstring ClientIniPath(bool createFolder) {
    PWSTR local = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)) || !local)
        return {};
    std::wstring dir(local);
    CoTaskMemFree(local);
    dir += L"\\SKR";
    if (createFolder && !CreateDirectoryW(dir.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS)
        return {};
    return dir + L"\\client.ini";
}

Prefs Clamp(Prefs p) {
    if (!std::isfinite(p.uiScale)) p.uiScale = 1.0f;
    p.uiScale = std::clamp(std::round(p.uiScale * 20.0f) / 20.0f, 0.75f, 1.50f);
    return p;
}

// Earlier builds kept only the UI scale in <gta>\SKR\ui_scale.cfg.
bool ReadLegacyUiScale(float& out) {
    char path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* slash = (n && n < MAX_PATH) ? strrchr(path, '\\') : nullptr;
    if (!slash) return false;
    snprintf(slash + 1, MAX_PATH - (slash + 1 - path), "SKR\\ui_scale.cfg");
    FILE* f = fopen(path, "r");
    if (!f) return false;
    float v = 1.0f;
    const bool ok = fscanf(f, "%f", &v) == 1;
    fclose(f);
    if (ok) out = v;
    return ok;
}

bool Write(const Prefs& p) {
    const std::wstring path = ClientIniPath(true);
    if (path.empty()) return false;
    wchar_t scale[16];
    swprintf(scale, 16, L"%.2f", p.uiScale);
    return WritePrivateProfileStringW(L"Interface", L"UiScale", scale, path.c_str()) &&
           WritePrivateProfileStringW(L"Interface", L"Animations", p.animations ? L"1" : L"0", path.c_str()) &&
           WritePrivateProfileStringW(L"Client", L"DebugLog", p.debugLog ? L"1" : L"0", path.c_str()) &&
           WritePrivateProfileStringW(L"Client", L"ShowVersion", p.showVersion ? L"1" : L"0", path.c_str());
}

void Load() {
    g_loaded = true;
    Prefs p;
    const std::wstring path = ClientIniPath(false);
    const bool exists = !path.empty() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (exists) {
        wchar_t buf[32] = {};
        GetPrivateProfileStringW(L"Interface", L"UiScale", L"", buf, 32, path.c_str());
        if (buf[0]) p.uiScale = static_cast<float>(wcstod(buf, nullptr));
        p.animations  = GetPrivateProfileIntW(L"Interface", L"Animations", 1, path.c_str()) != 0;
        p.debugLog    = GetPrivateProfileIntW(L"Client", L"DebugLog", 1, path.c_str()) != 0;
        p.showVersion = GetPrivateProfileIntW(L"Client", L"ShowVersion", 0, path.c_str()) != 0;
        g_prefs = Clamp(p);
    } else {
        // First run: migrate the legacy UI scale, then create client.ini.
        float legacy = 1.0f;
        if (ReadLegacyUiScale(legacy)) p.uiScale = legacy;
        g_prefs = Clamp(p);
        Write(g_prefs);
    }
}

constexpr unsigned kFpsCaps[] = { 0, 30, 60, 90, 120, 144, 165, 240 };  // performance::IsValidCap

} // namespace

const Prefs& Get() {
    if (!g_loaded) Load();
    return g_prefs;
}

bool Set(const Prefs& prefs) {
    g_loaded = true;
    g_prefs = Clamp(prefs);
    return Write(g_prefs);
}

bool DebugLogEnabled() {
    return Get().debugLog;
}

bool GetFpsCounter() { return performance::LoadFpsSettings().showCounter; }
unsigned GetFpsCap() { return performance::LoadFpsSettings().cap; }

bool SetFps(bool showCounter, unsigned cap) {
    performance::FpsSettings s;
    s.showCounter = showCounter;
    s.cap = performance::IsValidCap(cap) ? cap : 0;
    if (!performance::SaveFpsSettings(s)) return false;
    skr::render::fps::ReloadPreferences();   // render thread: same thread as the ESC menu
    return true;
}

const unsigned* FpsCapChoices(int& count) {
    count = static_cast<int>(sizeof(kFpsCaps) / sizeof(kFpsCaps[0]));
    return kFpsCaps;
}

bool ResetAll() {
    const bool a = Set(Prefs{});
    const bool b = SetFps(performance::FpsSettings{}.showCounter, performance::FpsSettings{}.cap);
    const bool c = skr::radar_hud::SetConfig(skr::radar_hud::Config{});
    return a && b && c;
}

} // namespace skr::native_pause::enhanced
