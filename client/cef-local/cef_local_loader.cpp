#include "cef-local/cef_local_loader.h"
#include "cef-bridge/cef_bridge.h"
#include "cef-bridge/cef_api_types.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>

using SetEscapeMenuModeFn = void(__cdecl*)(int);
using SetCustomMenuOpenFn = void(__cdecl*)(bool);
static SetEscapeMenuModeFn g_setEscapeMenuMode = nullptr;
static SetCustomMenuOpenFn g_setCustomMenuOpen = nullptr;

namespace skr::cef_local {

namespace {

std::atomic<bool> g_loaded{false};
std::mutex g_loadMutex;

// Resolved InternalApi that bridges to omp-cef exports.
// Must outlive CefBridge (static lifetime).
skr::cef::InternalApi g_api{};

std::wstring LogDir() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&LogDir), &self);
    wchar_t p[MAX_PATH];
    DWORD n = GetModuleFileNameW(self, p, MAX_PATH);
    if (!n || n >= MAX_PATH) return {};
    std::wstring s(p, n);
    auto slash = s.find_last_of(L'\\');
    if (slash == std::wstring::npos) return {};
    return s.substr(0, slash + 1) + L"SKR\\";
}

void Log(const char* fmt, ...) {
    std::wstring dir = LogDir();
    if (dir.empty()) return;
    CreateDirectoryW(dir.c_str(), nullptr);
    FILE* f = _wfopen((dir + L"cef_local.log").c_str(), L"a");
    if (!f) return;
    fprintf(f, "[%lu] ", GetTickCount());
    va_list ap; va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

template <typename T>
bool Resolve(HMODULE mod, const char* name, T& out) {
    auto* proc = GetProcAddress(mod, name);
    if (!proc) {
        Log("FAILED to resolve export: %s", name);
        return false;
    }
    out = reinterpret_cast<T>(proc);
    return true;
}

} // namespace

bool TryLoad() {
    std::lock_guard<std::mutex> lk(g_loadMutex);
    if (g_loaded.load()) return true;

    // omp-cef's client.dll lives in <gta_dir>/cef/client.dll.
    // It's loaded by cef.asi before we get here; try GetModuleHandle first.
    HMODULE mod = GetModuleHandleW(L"client.dll");
    if (!mod) {
        // Load from <gta_dir>/cef/client.dll.
        // client.dll depends on libcef.dll which lives in the same cef/ folder.
        // Resolve siblings without changing the process-wide DLL search path.
        wchar_t exe[MAX_PATH]{};
        DWORD length = GetModuleFileNameW(nullptr, exe, MAX_PATH);
        if (!length || length >= MAX_PATH) return false;
        std::wstring dir(exe);
        auto slash = dir.rfind(L'\\');
        if (slash != std::wstring::npos) dir.resize(slash);
        std::wstring cefDir = dir + L"\\cef";
        std::wstring path = cefDir + L"\\client.dll";
        Log("loading omp-cef from %ls", path.c_str());
        mod = LoadLibraryExW(path.c_str(), nullptr,
                             LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!mod) {
            DWORD err = GetLastError();
            Log("LoadLibraryW failed: error %lu", err);
        }
    }

    if (!mod) {
        Log("omp-cef client.dll not found");
        return false;
    }

    Log("omp-cef client.dll found at 0x%p", mod);

    bool ok = true;
    ok &= Resolve(mod, "skr_cef_create_browser",   g_api.cef_create_browser);
    ok &= Resolve(mod, "skr_cef_destroy_browser",   g_api.cef_destroy_browser);
    ok &= Resolve(mod, "skr_cef_hide_browser",      g_api.cef_hide_browser);
    ok &= Resolve(mod, "skr_cef_focus_browser",      g_api.cef_focus_browser);
    ok &= Resolve(mod, "skr_cef_create_list",        g_api.cef_create_list);
    ok &= Resolve(mod, "skr_cef_emit_event",         g_api.cef_emit_event);
    ok &= Resolve(mod, "skr_cef_subscribe",          g_api.cef_subscribe);
    ok &= Resolve(mod, "skr_cef_input_available",    g_api.cef_input_available);
    ok &= Resolve(mod, "skr_cef_ready",              g_api.cef_ready);
    ok &= Resolve(mod, "skr_cef_try_focus_browser",  g_api.cef_try_focus_browser);
    ok &= Resolve(mod, "skr_cef_browser_exists",     g_api.cef_browser_exists);
    ok &= Resolve(mod, "skr_cef_browser_ready",      g_api.cef_browser_ready);
    ok &= Resolve(mod, "skr_cef_on_browser_ready",   g_api.cef_on_browser_ready);
    ok &= Resolve(mod, "skr_cef_gta_window_active",  g_api.cef_gta_window_active);

    if (!ok) {
        Log("one or more omp-cef exports missing — falling back to samp-cef path");
        memset(&g_api, 0, sizeof(g_api));
        return false;
    }

    // Optional (newer omp-cef builds): string-arg emit for C++ -> JS payloads.
    skr::cef::EmitEventStrFn emitStr = reinterpret_cast<skr::cef::EmitEventStrFn>(
        GetProcAddress(mod, "skr_cef_emit_event_str"));
    if (!emitStr) Log("skr_cef_emit_event_str missing — C++ -> JS events carry no args");

    // Optional: escape menu mode control.
    g_setEscapeMenuMode = reinterpret_cast<SetEscapeMenuModeFn>(
        GetProcAddress(mod, "skr_cef_set_escape_menu_mode"));
    if (!g_setEscapeMenuMode) Log("skr_cef_set_escape_menu_mode missing — ESC menu hook unavailable");

    g_setCustomMenuOpen = reinterpret_cast<SetCustomMenuOpenFn>(
        GetProcAddress(mod, "skr_cef_set_custom_menu_open"));
    if (!g_setCustomMenuOpen) Log("skr_cef_set_custom_menu_open missing");

    // Wire it up to CefBridge — exactly as samp-cef would via cef_initialize().
    skr::cef::CefBridge::Instance().InitializeLocal(&g_api, emitStr);
    g_loaded.store(true);
    Log("CefBridge initialized via omp-cef local API");
    return true;
}

bool IsLoaded() {
    return g_loaded.load();
}

void SetEscapeMenuMode(int mode) {
    if (g_setEscapeMenuMode) g_setEscapeMenuMode(mode);
}

void SetCustomMenuOpen(bool open) {
    if (g_setCustomMenuOpen) g_setCustomMenuOpen(open);
}

} // namespace skr::cef_local
