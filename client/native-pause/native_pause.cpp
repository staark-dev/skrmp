// ═══════════════════════════════════════════════════════════════════════
// SKR Native Pause Menu — Core Module
// Task #22: Replace CEF-based Escape Menu with native C++/DX9 pause menu
// ═══════════════════════════════════════════════════════════════════════

#include "native-pause/native_pause.h"
#include "native-pause/pause_enhanced.h"
#include "native-pause/pause_renderer.h"
#include "native-pause/pause_input.h"
#include "native-pause/pause_actions.h"
#include "gps/gps_panel.h"
#include "phone/phone_input.h"
#include "native-api/native_api_ui.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#include <atomic>

namespace skr::native_pause {
namespace {

// ── Log helpers ─────────────────────────────────────────────────────
static char g_logPath[MAX_PATH] = {};
static bool g_logPathOk = false;

static const char* GetLogPath() {
    if (g_logPathOk) return g_logPath;
    DWORD n = GetModuleFileNameA(nullptr, g_logPath, MAX_PATH);
    if (!n || n >= MAX_PATH) return nullptr;
    char* slash = strrchr(g_logPath, '\\');
    if (!slash) return nullptr;
    snprintf(slash + 1, MAX_PATH - (slash + 1 - g_logPath), "SKR\\native_pause.log");
    g_logPathOk = true;
    return g_logPath;
}

void Log(const char* fmt, ...) {
    if (!skr::native_pause::enhanced::DebugLogEnabled()) return;
    const char* path = GetLogPath();
    if (!path) return;
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

// ── State ───────────────────────────────────────────────────────────
std::atomic<bool> g_initialized{false};
HWND g_gtaWindow = nullptr;

// GTA SA 1.0 US: RsGlobal.ps->window (the GTA HWND)
// The window is found by enumerating top-level windows for gta_sa.exe's thread.
HWND FindGtaWindow() {
    // Use the foreground window heuristic — GTA's window is the main visible one
    // on its process. We can also use the known global at 0x00C97C1C (RwCamera)
    // but the simplest approach for the HWND is FindWindow with the known class.
    HWND wnd = FindWindowA("Grand theft auto San Andreas", nullptr);
    if (!wnd) {
        // SA-MP may change the window title, try class name
        wnd = FindWindowA(nullptr, "GTA:SA:MP");
    }
    return wnd;
}

} // namespace

bool Initialize() {
    if (g_initialized.load()) return true;

    Log("Initialize: starting native pause menu setup");

    g_gtaWindow = FindGtaWindow();
    if (!g_gtaWindow) {
        Log("Initialize: GTA window not found (will retry on first EndScene)");
        // Not fatal — we'll find it on first render
    }

    g_initialized.store(true);
    Log("Initialize: native pause menu ready (window=0x%p)", g_gtaWindow);
    return true;
}

void Shutdown() {
    if (!g_initialized.load()) return;

    Log("Shutdown: cleaning up native pause menu");

    input::Shutdown();
    renderer::Shutdown();

    g_initialized.store(false);
    g_gtaWindow = nullptr;
    Log("Shutdown: complete");
}

void OnEndScene(IDirect3DDevice9* device) {
    if (!g_initialized.load() || !device) return;

    // Lazy initialization of window, renderer, and input on first frame
    if (!g_gtaWindow) {
        g_gtaWindow = FindGtaWindow();
        if (!g_gtaWindow) return;  // Still not ready
        Log("OnEndScene: found GTA window 0x%p", g_gtaWindow);
    }

    if (!renderer::IsReady()) {
        if (!renderer::Initialize(device, g_gtaWindow)) {
            Log("OnEndScene: renderer init failed");
            return;
        }
        Log("OnEndScene: renderer initialized");
    }

    // Initialize input after we have the window
    static bool s_inputInit = false;
    if (!s_inputInit) {
        if (input::Initialize(g_gtaWindow)) {
            s_inputInit = true;
            Log("OnEndScene: input handler initialized");
        }
    }

    // Initialize phone input on first frame (after renderer + input are ready).
    static bool s_phoneInit = false;
    if (!s_phoneInit && renderer::IsReady()) {
        skr::phone::input::Initialize();
        s_phoneInit = true;
    }

    // Keep SA-MP's cursor mode synchronized before processing menu actions.
    input::Tick();

    // Phone per-frame tick (animations, cursor, death detection).
    skr::phone::input::Tick(GetTickCount64());

    // Tick actions (monitors native submenu lifecycle)
    actions::Tick();

    // One existing ImGui owner serves both surfaces. The GPS navigation panel
    // and phone render during gameplay; the pause menu stays exclusive while it
    // is open so the three never overlap.
    const bool pauseOpen = input::GetState() == input::PauseState::PauseOpen;
    if (pauseOpen || skr::gps::panel::WantsRender() || skr::phone::input::WantsRender()
        || skr::native_api::ui::WantsRender())
        renderer::Render(device, pauseOpen);
}

void OnBeforeReset() {
    renderer::OnBeforeReset();
}

void OnAfterReset(IDirect3DDevice9* device) {
    renderer::OnAfterReset(device);
}

bool OnWndProc(unsigned int msg, uintptr_t wParam, intptr_t lParam) {
    if (!g_initialized.load()) return false;
    return input::ProcessMessage(msg, static_cast<WPARAM>(wParam), static_cast<LPARAM>(lParam));
}

bool IsOpen() {
    return g_initialized.load() &&
           input::GetState() != input::PauseState::Gameplay;
}

bool IsInitialized() {
    return g_initialized.load();
}

} // namespace skr::native_pause
