#include "gps_flags.h"
#include "gps_engine.h"
#include "gps_renderer.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

namespace skr::gps {

void Log(const char* fmt, ...) {
    char path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* slash = (n && n < MAX_PATH) ? strrchr(path, '\\') : nullptr;
    if (!slash) return;
    snprintf(slash + 1, MAX_PATH - (slash + 1 - path), "SKR\\gps.log");
    FILE* f = fopen(path, "a");
    if (!f) return;
    fprintf(f, "[%lu] ", GetTickCount());
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fprintf(f, "\n");
    fclose(f);
}

namespace flags {
namespace {

constexpr DWORD kPollMs = 1000;

Flags    g_flags;
bool     g_loaded = false;
DWORD    g_lastPoll = 0;
FILETIME g_stamp = {};

std::wstring IniPath(bool createFolder) {
    PWSTR local = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)) || !local)
        return {};
    std::wstring dir(local);
    CoTaskMemFree(local);
    dir += L"\\SKR";
    if (createFolder && !CreateDirectoryW(dir.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS)
        return {};
    return dir + L"\\gps_debug.ini";
}

bool ReadStamp(const std::wstring& path, FILETIME& out) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return false;
    out = data.ftLastWriteTime;
    return true;
}

void Apply(const Flags& next, const char* source) {
    const Transition t = Diff(g_flags, next);
    g_flags = next;
    if (t.resetCore) Reset();
    if (t.resetRenderer) ResetRenderer();
    Log("flags (%s): core=%d renderer=%d waypoints=%d zoom=%d%s%s", source,
        next.core ? 1 : 0, next.renderer ? 1 : 0, next.waypoints ? 1 : 0, next.zoom ? 1 : 0,
        t.resetCore ? " [core reset]" : "", t.resetRenderer ? " [renderer reset]" : "");
}

void Poll() {
    const DWORD now = GetTickCount();
    if (g_loaded && now - g_lastPoll < kPollMs) return;
    g_lastPoll = now;

    std::wstring path = IniPath(!g_loaded);
    if (path.empty()) {
        if (!g_loaded) { g_loaded = true; Apply(Flags{}, "defaults, no LocalAppData"); }
        return;
    }
    FILETIME stamp{};
    if (!ReadStamp(path, stamp)) {
        if (g_loaded) return;
        // First run: create an all-disabled template the tester can edit.
        WritePrivateProfileStringW(L"GPS", L"Core", L"0", path.c_str());
        WritePrivateProfileStringW(L"GPS", L"Renderer", L"0", path.c_str());
        WritePrivateProfileStringW(L"GPS", L"Waypoints", L"0", path.c_str());
        WritePrivateProfileStringW(L"GPS", L"AdaptiveZoom", L"0", path.c_str());
        ReadStamp(path, g_stamp);
        g_loaded = true;
        Apply(Flags{}, "defaults, template created");
        return;
    }
    if (g_loaded && CompareFileTime(&stamp, &g_stamp) == 0) return;
    g_stamp = stamp;

    Flags next;
    next.core      = GetPrivateProfileIntW(L"GPS", L"Core", 0, path.c_str()) != 0;
    next.renderer  = GetPrivateProfileIntW(L"GPS", L"Renderer", 0, path.c_str()) != 0;
    next.waypoints = GetPrivateProfileIntW(L"GPS", L"Waypoints", 0, path.c_str()) != 0;
    next.zoom      = GetPrivateProfileIntW(L"GPS", L"AdaptiveZoom", 0, path.c_str()) != 0;
    const bool first = !g_loaded;
    g_loaded = true;
    Apply(next, first ? "gps_debug.ini" : "gps_debug.ini changed");
}

} // namespace

namespace { std::optional<Flags> g_fixed; }

bool CoreEnabled()      { if (g_fixed) return g_fixed->core;      Poll(); return g_flags.core; }
// Render-path queries never touch the file system; the first load happens
// on the first game tick (CoreEnabled) or here if a renderer asks earlier.
bool CoreEnabledCached() { if (g_fixed) return g_fixed->core;     if (!g_loaded) Poll(); return g_flags.core; }
bool RendererEnabled()  { if (g_fixed) return g_fixed->renderer;  if (!g_loaded) Poll(); return g_flags.renderer; }
bool WaypointsEnabled() { if (g_fixed) return g_fixed->waypoints; if (!g_loaded) Poll(); return g_flags.waypoints; }
bool ZoomEnabled()      { if (g_fixed) return g_fixed->zoom;      if (!g_loaded) Poll(); return g_flags.zoom; }

void SetForTesting(std::optional<Flags> fixed) { g_fixed = fixed; }

} // namespace flags
} // namespace skr::gps
