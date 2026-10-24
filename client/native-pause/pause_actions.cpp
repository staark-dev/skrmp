// ═══════════════════════════════════════════════════════════════════════
// SKR Native Pause Menu — Actions
// Resume, Exit, native MAP/SETTINGS submenu integration, disconnect.
// Reuses CMenuManager patterns from escape_menu.cpp.
//
// FIXED: Disconnect now uses ExitProcess(kExitRestartLobby) matching
// escape_menu.cpp, instead of WM_CLOSE which doesn't return to lobby.
// ═══════════════════════════════════════════════════════════════════════

#include "native-pause/pause_actions.h"
#include "native-pause/pause_enhanced.h"
#include "native-pause/pause_input.h"
#include "native-pause/pause_settings.h"
#include "../../shared/exit_codes.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdint>

namespace skr::native_pause::actions {
namespace {

// ── Log ─────────────────────────────────────────────────────────────
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

static void Log(const char* fmt, ...) {
    if (!skr::native_pause::enhanced::DebugLogEnabled()) return;
    const char* path = GetLogPath();
    if (!path) return;
    FILE* f = fopen(path, "a");
    if (!f) return;
    fprintf(f, "[%lu] ACTIONS: ", GetTickCount());
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fprintf(f, "\n");
    fclose(f);
}

// ── GTA SA 1.0 US CMenuManager addresses ────────────────────────────
// Provenance: escape_menu.cpp, confirmed against plugin-sdk/gta-reversed.
constexpr uintptr_t kFrontEndMenuManager   = 0x00BA6748;
constexpr uintptr_t kMenuActive            = kFrontEndMenuManager + 0x5C;
constexpr uintptr_t kDontDrawFrontEnd      = kFrontEndMenuManager + 0x32;
constexpr uintptr_t kActivateMenuNextFrame = kFrontEndMenuManager + 0x33;
constexpr uintptr_t kCurrentMenuPage       = kFrontEndMenuManager + 0x15D;
constexpr uint8_t   kMenuPageOptions       = 33;
constexpr uint8_t   kMenuPageMap           = 5;
// eMenuPage (deps/omp-cef/src/client/core/game_sa/CMenuManager.h)
constexpr uint8_t   kMenuPageMainMenu      = 34;  // MENUPAGE_MAIN_MENU
constexpr uint8_t   kMenuPagePauseMenu     = 41;  // MENUPAGE_PAUSE_MENU

// ── Native submenu lifecycle ────────────────────────────────────────
// When opening a native submenu (MAP or SETTINGS), we:
// 1. Restore GTA's menu system temporarily
// 2. Set the desired menu page
// 3. Activate the menu next frame
// When the native menu closes, we re-patch and return to PauseOpen.

bool g_waitingForNativeMenu = false;
bool g_nativeMenuBecameActive = false;
DWORD g_nativeMenuRequestTime = 0;
uint8_t g_requestedPage = 0;
DWORD g_nativeMenuFrames = 0;   // Tick() frames observed with the menu active
// If GTA never shows the requested page (e.g. the frontend refused to
// activate), return to the SKR pause surface instead of leaving the player
// in NativeSubmenu state with no visible UI.
constexpr DWORD kNativeMenuActivationTimeoutMs = 2000;

void OpenNativeMenuAt(uint8_t page) {
    Log("opening native menu at page %u", page);

    // The custom surface already sits on GTA's real paused state. Hand the
    // existing state back to GTA and reveal only the requested native page.
    *reinterpret_cast<uint8_t*>(kCurrentMenuPage) = page;
    *reinterpret_cast<uint8_t*>(kDontDrawFrontEnd) = 0;
    if (!*reinterpret_cast<uint8_t*>(kMenuActive))
        *reinterpret_cast<uint8_t*>(kActivateMenuNextFrame) = 1;

    g_waitingForNativeMenu = true;
    g_nativeMenuBecameActive = *reinterpret_cast<uint8_t*>(kMenuActive) != 0;
    g_nativeMenuRequestTime = GetTickCount();
    g_requestedPage = page;
    g_nativeMenuFrames = 0;

    input::SetState(input::PauseState::NativeSubmenu);
}

} // namespace

void Resume() {
    Log("resume: returning to gameplay");
    input::SetState(input::PauseState::Gameplay);

}

void OpenNativeMap() {
    OpenNativeMenuAt(kMenuPageMap);
}

void OpenNativeSettings() {
    OpenNativeMenuAt(kMenuPageOptions);
}

void Disconnect() {
    Log("disconnect: ExitProcess(%u) — returning to lobby", skr::kExitRestartLobby);
    // Match escape_menu.cpp: exit with the restart-lobby code so the
    // launcher relaunches with server selection instead of just closing.
    ExitProcess(skr::kExitRestartLobby);
}

void ExitGame() {
    Log("exit game requested");
    input::SetState(input::PauseState::Gameplay);

    HWND wnd = FindWindowA("Grand theft auto San Andreas", nullptr);
    if (!wnd) wnd = FindWindowA(nullptr, "GTA:SA:MP");
    if (wnd) {
        PostMessageA(wnd, WM_CLOSE, 0, 0);
    } else {
        TerminateProcess(GetCurrentProcess(), 0);
    }
}

void Tick() {
    // Resolution keep/revert timeout runs even when the menu is closed.
    settings::Tick();

    if (!g_waitingForNativeMenu) return;

    // Do not treat the activation frame as a close. Wait until GTA has shown
    // the requested page at least once, then return after it becomes inactive.
    uint8_t menuActive = *reinterpret_cast<uint8_t*>(kMenuActive);
    if (menuActive != 0) {
        g_nativeMenuBecameActive = true;
        ++g_nativeMenuFrames;

        // Return to the SKR surface as soon as the player backs out of the
        // requested native page, instead of leaving GTA's own pause page on
        // screen. MAP: any page change is "back". Options tree: backing out to
        // GTA's pause/main page. A couple of frames are skipped so GTA has
        // processed the page we wrote. If neither happens, the menu-closed
        // path below still applies.
        if (g_nativeMenuFrames > 2 && input::GetState() == input::PauseState::NativeSubmenu) {
            const uint8_t page = *reinterpret_cast<uint8_t*>(kCurrentMenuPage);
            const bool left = (g_requestedPage == kMenuPageMap)
                ? page != kMenuPageMap
                : (page == kMenuPagePauseMenu || page == kMenuPageMainMenu);
            if (left) {
                Log("native page %u left (now %u), returning to pause menu",
                    g_requestedPage, page);
                g_waitingForNativeMenu = false;
                g_nativeMenuBecameActive = false;
                settings::Refresh();
                input::SetState(input::PauseState::PauseOpen);
            }
        }
        return;
    }

    if (!g_nativeMenuBecameActive &&
        input::GetState() == input::PauseState::NativeSubmenu &&
        GetTickCount() - g_nativeMenuRequestTime > kNativeMenuActivationTimeoutMs) {
        Log("native submenu did not activate within %lu ms, returning to pause menu",
            kNativeMenuActivationTimeoutMs);
        g_waitingForNativeMenu = false;
        input::SetState(input::PauseState::PauseOpen);
        return;
    }

    if (g_nativeMenuBecameActive && input::GetState() == input::PauseState::NativeSubmenu) {
        Log("native submenu closed, returning to pause menu");

        g_waitingForNativeMenu = false;
        g_nativeMenuBecameActive = false;
        settings::Refresh();
        input::SetState(input::PauseState::PauseOpen);
    }
}

} // namespace skr::native_pause::actions
