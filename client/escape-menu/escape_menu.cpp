#include "escape-menu/escape_menu.h"
#include "cef-bridge/cef_bridge.h"
#include "cef-local/cef_local_loader.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>

namespace skr::escape_menu {

namespace {

std::mutex g_logMutex;

std::wstring DllDir() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&DllDir), &self);
    wchar_t p[MAX_PATH];
    DWORD n = GetModuleFileNameW(self, p, MAX_PATH);
    if (!n || n >= MAX_PATH) return {};
    std::wstring s(p, n);
    auto slash = s.find_last_of(L'\\');
    if (slash == std::wstring::npos) return {};
    return s.substr(0, slash + 1) + L"SKR\\";
}

void Log(const char* fmt, ...) {
    std::wstring dir = DllDir();
    if (dir.empty()) return;
    CreateDirectoryW(dir.c_str(), nullptr);
    std::lock_guard<std::mutex> lk(g_logMutex);
    FILE* f = _wfopen((dir + L"escape_menu.log").c_str(), L"a");
    if (!f) return;
    fprintf(f, "[%lu] ", GetTickCount());
    va_list ap; va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

constexpr uint32_t kBrowserId = skr::cef::BrowserId::kEscMenu;
constexpr int kEscapeModeCustom = 2;
constexpr int kEscapeModeGta    = 0;

std::atomic<bool> g_enabled{false};
std::atomic<bool> g_browserCreated{false};
std::atomic<bool> g_menuOpen{false};
std::atomic<bool> g_eventsSubscribed{false};

std::string MenuUrl() {
    std::wstring dir = DllDir();
    if (dir.empty()) return {};
    std::wstring full = dir + L"escape_menu\\escape_menu.html";
    std::wstring url = L"file:///";
    for (wchar_t c : full) url += (c == L'\\' ? L'/' : c);
    int n = WideCharToMultiByte(CP_UTF8, 0, url.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, url.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

// JS "Resume" button -> C++: hide browser, tell omp-cef the menu is closed.
int __cdecl OnResume(const char*, skr::cef::CefListValue*) {
    auto& bridge = skr::cef::CefBridge::Instance();
    if (g_browserCreated.load()) bridge.HideBrowser(kBrowserId);
    g_menuOpen.store(false);
    skr::cef_local::SetCustomMenuOpen(false);
    Log("resume from menu");
    return 0;
}

int __cdecl OnSettings(const char*, skr::cef::CefListValue*) {
    Log("settings requested from escape menu");
    return 0;
}

int __cdecl OnDisconnect(const char*, skr::cef::CefListValue*) {
    Log("disconnect requested from escape menu");
    HWND w = FindWindowW(L"Grand theft auto San Andreas", nullptr);
    if (w) PostMessageW(w, WM_CLOSE, 0, 0);
    ExitProcess(0);  // [[noreturn]]
}

void SubscribeOnce() {
    if (g_eventsSubscribed.exchange(true)) return;
    auto& bridge = skr::cef::CefBridge::Instance();
    bridge.Subscribe("skr:esc:resume",      &OnResume);
    bridge.Subscribe("skr:esc:settings",    &OnSettings);
    bridge.Subscribe("skr:esc:disconnect",  &OnDisconnect);
    Log("escape menu event handlers subscribed");
}

} // namespace

void Enable() {
    if (g_enabled.exchange(true)) return;
    auto& bridge = skr::cef::CefBridge::Instance();

    skr::cef_local::SetEscapeMenuMode(kEscapeModeCustom);
    SubscribeOnce();

    // Pre-create the browser hidden so it's ready when ESC is pressed.
    // omp-cef's EscapeMenuController will emit "cef:escape_menu" (bool) to
    // all browsers including this one; the JS page handles show/hide.
    if (!g_browserCreated.load() && bridge.IsReady()) {
        std::string url = MenuUrl();
        if (!url.empty()) {
            bridge.CreateBrowser(kBrowserId, url.c_str(), /*hidden*/ true, /*focused*/ false);
            g_browserCreated.store(true);
            Log("escape menu browser pre-created (hidden): %s", url.c_str());
        }
    }

    Log("escape menu enabled (custom mode)");
}

void Disable() {
    if (!g_enabled.exchange(false)) return;
    skr::cef_local::SetEscapeMenuMode(kEscapeModeGta);
    auto& bridge = skr::cef::CefBridge::Instance();
    if (g_browserCreated.exchange(false)) {
        bridge.DestroyBrowser(kBrowserId);
    }
    g_menuOpen.store(false);
    Log("escape menu disabled (native mode restored)");
}

void Tick() {
    if (!g_enabled.load()) return;
    auto& bridge = skr::cef::CefBridge::Instance();
    if (!bridge.IsReady()) return;

    // Lazy browser creation if Enable() was called before CEF was ready.
    if (!g_browserCreated.load()) {
        std::string url = MenuUrl();
        if (!url.empty()) {
            bridge.CreateBrowser(kBrowserId, url.c_str(), true, false);
            g_browserCreated.store(true);
            Log("escape menu browser created (deferred): %s", url.c_str());
        }
    }

    // Zoom the menu page for high-res displays.
    if (g_browserCreated.load() && g_menuOpen.load()) {
        using ZoomFn = void(__cdecl*)(uint32_t, double);
        static ZoomFn pZoom = reinterpret_cast<ZoomFn>(
            GetProcAddress(GetModuleHandleW(L"client.dll"), "skr_cef_set_zoom"));
        static unsigned zoomTick = 0;
        if (pZoom && (zoomTick++ % 120) == 0) {
            HWND hwnd = FindWindowW(L"Grand theft auto San Andreas", nullptr);
            RECT rc{};
            if (hwnd && GetClientRect(hwnd, &rc) && rc.bottom > 0) {
                double factor = static_cast<double>(rc.bottom) / 768.0;
                if (factor < 1.0) factor = 1.0;
                if (factor > 2.5) factor = 2.5;
                pZoom(kBrowserId, factor);
            }
        }
    }
}

bool IsEnabled() { return g_enabled.load(); }
bool IsOpen()    { return g_menuOpen.load(); }

} // namespace skr::escape_menu
