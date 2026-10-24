// ═══════════════════════════════════════════════════════════════════════
// SKR Native Pause Menu — Input Handler
// ESC key interception, state machine, Win32 message processing.
// Integrates with SA-MP's DirectInput by suppressing GTA's native
// CMenuManager activation and handling ESC ourselves.
//
// FIXED: ImGui_ImplWin32_WndProcHandler is now called when the pause
// menu is open so ImGui receives mouse/keyboard events for interaction.
// ═══════════════════════════════════════════════════════════════════════

#include "native-pause/pause_input.h"
#include "native-pause/pause_enhanced.h"
#include "native-pause/pause_actions.h"
#include "native-pause/pause_controls.h"
#include "native-pause/pause_renderer.h"
#include "native-pause/pause_settings.h"
#include "lobby/menu_patch.h"
#include "lobby/lobby_manager.h"
#include "native-api/native_api.h"
#include "phone/phone_input.h"

#include <windows.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <atomic>
#include <cstdio>
#include <cstring>

// ImGui Win32 handler — needed for mouse hover, click, scroll, keyboard
// Forward-declared without imgui headers; the linker resolves it from imgui_impl_win32.cpp.
extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace skr::native_pause::input {
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
    fprintf(f, "[%lu] INPUT: ", GetTickCount());
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fprintf(f, "\n");
    fclose(f);
}

// ── State ───────────────────────────────────────────────────────────
PauseState g_state = PauseState::Gameplay;
PauseTab   g_activeTab = PauseTab::Game;
MouseState g_mouse = {};
HWND       g_window = nullptr;
WNDPROC    g_origWndProc = nullptr;
bool       g_initialized = false;
bool       g_cursorOwned = false;

// SA-MP 0.3.7-R5-1 input ownership. These RVAs and the cursor-mode values are
// the same ones used by the pinned omp-cef GameView_R5/FocusManager sources:
//   deps/omp-cef/src/client/core/samp/components/r5/game.cpp
//   deps/omp-cef/src/client/core/samp/common.hpp
constexpr DWORD     kSampR5Timestamp             = 0x6372C39E;
constexpr DWORD     kSampR5ImageSize             = 0x27E000;
constexpr uintptr_t kSampR5GameRva               = 0x26EBAC;
constexpr uintptr_t kSampR5SetCursorModeRva      = 0x0A06F0;
constexpr uintptr_t kSampR5ProcessInputRva       = 0x0A05D0;
constexpr int       kSampCursorNone               = 0;
constexpr int       kSampCursorLockCamAndControl  = 2;

// GTA SA 1.0 US: CMenuManager addresses (from escape_menu.cpp)
constexpr uintptr_t kFrontEndMenuManager   = 0x00BA6748;
constexpr uintptr_t kMenuActive            = kFrontEndMenuManager + 0x5C;
constexpr uintptr_t kDontDrawFrontEnd      = kFrontEndMenuManager + 0x32;
constexpr uintptr_t kActivateMenuNextFrame = kFrontEndMenuManager + 0x33;
constexpr uintptr_t kUserPause              = 0x00B7CB49;

// GTA SA 1.0 US CTimer::StartUserPause / EndUserPause. Provenance:
// deps/omp-cef/src/client/core/game_sa/CTimer.cpp in this workspace.
using TimerPauseFn = void(__cdecl*)();
constexpr uintptr_t kStartUserPause = 0x00561AF0;
constexpr uintptr_t kEndUserPause   = 0x00561B00;

// Debounce: prevent ESC from toggling rapidly
// Keyboard navigation queue (WndProc → renderer, both on the GTA thread).
constexpr int kNavQueueSize = 16;
NavEvent g_navQueue[kNavQueueSize] = {};
int g_navHead = 0;
int g_navCount = 0;

void PushNav(NavEvent ev) {
    if (g_navCount >= kNavQueueSize) return;
    g_navQueue[(g_navHead + g_navCount) % kNavQueueSize] = ev;
    ++g_navCount;
}

void ClearNav() {
    g_navHead = 0;
    g_navCount = 0;
}

DWORD g_lastEscTime = 0;
constexpr DWORD kEscDebounceMs = 200;

// ── DirectInput ownership ───────────────────────────────────────────
using DIAcquireFn  = HRESULT(WINAPI*)(IDirectInputDevice8A*);
using DIGetStateFn = HRESULT(WINAPI*)(IDirectInputDevice8A*, DWORD, LPVOID);
DIAcquireFn  g_origDIAcquire = nullptr;
DIGetStateFn g_origDIGetState = nullptr;
void**       g_diVtable = nullptr;

bool ShouldOwnDirectInput() {
    return g_state == PauseState::PauseOpen || skr::lobby::IsLobbyActive() ||
           skr::phone::input::WantsDirectInput();
}

HRESULT WINAPI HookedDIAcquire(IDirectInputDevice8A* device) {
    if (ShouldOwnDirectInput()) return DI_OK;
    return g_origDIAcquire ? g_origDIAcquire(device) : DIERR_NOTINITIALIZED;
}

HRESULT WINAPI HookedDIGetDeviceState(IDirectInputDevice8A* device, DWORD size, LPVOID data) {
    if (ShouldOwnDirectInput()) {
        device->Unacquire();
        if (data) memset(data, 0, size);
        return DI_OK;
    }
    if (!g_origDIGetState) return DIERR_NOTINITIALIZED;
    HRESULT hr = g_origDIGetState(device, size, data);
    if ((hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) && g_origDIAcquire) {
        if (SUCCEEDED(g_origDIAcquire(device)))
            hr = g_origDIGetState(device, size, data);
    }
    return hr;
}

bool InstallDirectInputOwnership() {
    HMODULE module = GetModuleHandleW(L"dinput8.dll");
    using CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    auto create = module ? reinterpret_cast<CreateFn>(GetProcAddress(module, "DirectInput8Create")) : nullptr;
    if (!create) {
        Log("DirectInput ownership unavailable: DirectInput8Create missing");
        return false;
    }

    IDirectInput8A* di = nullptr;
    if (FAILED(create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8A,
                      reinterpret_cast<void**>(&di), nullptr)) || !di) {
        Log("DirectInput ownership unavailable: DirectInput8Create failed");
        return false;
    }

    IDirectInputDevice8A* mouse = nullptr;
    const HRESULT created = di->CreateDevice(GUID_SysMouse, &mouse, nullptr);
    di->Release();
    if (FAILED(created) || !mouse) {
        Log("DirectInput ownership unavailable: SysMouse creation failed");
        return false;
    }

    g_diVtable = *reinterpret_cast<void***>(mouse);
    DWORD old = 0;
    if (!VirtualProtect(&g_diVtable[7], 3 * sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
        mouse->Release();
        g_diVtable = nullptr;
        Log("DirectInput ownership unavailable: VirtualProtect failed");
        return false;
    }
    g_origDIAcquire = reinterpret_cast<DIAcquireFn>(g_diVtable[7]);
    g_origDIGetState = reinterpret_cast<DIGetStateFn>(g_diVtable[9]);
    g_diVtable[7] = reinterpret_cast<void*>(&HookedDIAcquire);
    g_diVtable[9] = reinterpret_cast<void*>(&HookedDIGetDeviceState);
    VirtualProtect(&g_diVtable[7], 3 * sizeof(void*), old, &old);
    FlushInstructionCache(GetCurrentProcess(), &g_diVtable[7], 3 * sizeof(void*));
    mouse->Release();
    Log("DirectInput ownership installed");
    return true;
}

void RemoveDirectInputOwnership() {
    if (!g_diVtable) return;
    DWORD old = 0;
    if (VirtualProtect(&g_diVtable[7], 3 * sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
        if (g_diVtable[7] == reinterpret_cast<void*>(&HookedDIAcquire))
            g_diVtable[7] = reinterpret_cast<void*>(g_origDIAcquire);
        if (g_diVtable[9] == reinterpret_cast<void*>(&HookedDIGetDeviceState))
            g_diVtable[9] = reinterpret_cast<void*>(g_origDIGetState);
        VirtualProtect(&g_diVtable[7], 3 * sizeof(void*), old, &old);
    }
    g_diVtable = nullptr;
    g_origDIAcquire = nullptr;
    g_origDIGetState = nullptr;
}

void ShowPauseCursor() {
    ClipCursor(nullptr);
    ReleaseCapture();
    SetCursor(LoadCursor(nullptr, IDC_ARROW));
    if (!g_cursorOwned) {
        while (ShowCursor(TRUE) < 0) {}
        g_cursorOwned = true;
    }
}

void HidePauseCursor() {
    if (!g_cursorOwned) return;
    while (ShowCursor(FALSE) >= 0) {}
    SetCursor(nullptr);
    g_cursorOwned = false;
}

bool IsPinnedSampR5(HMODULE module) {
    if (!module) return false;

    const auto* base = reinterpret_cast<const uint8_t*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
        return false;

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE &&
           nt->FileHeader.TimeDateStamp == kSampR5Timestamp &&
           nt->OptionalHeader.SizeOfImage == kSampR5ImageSize;
}

void SetSampInputOwnership(bool owned) {
    HMODULE samp = GetModuleHandleW(L"samp.dll");
    if (!IsPinnedSampR5(samp)) return;

    auto* base = reinterpret_cast<uint8_t*>(samp);
    void* game = *reinterpret_cast<void**>(base + kSampR5GameRva);
    if (!game) return;

    using SetCursorModeFn = void(__thiscall*)(void*, int, int);
    using ProcessInputFn = void(__thiscall*)(void*);
    const auto setCursorMode = reinterpret_cast<SetCursorModeFn>(
        base + kSampR5SetCursorModeRva);
    const auto processInput = reinterpret_cast<ProcessInputFn>(
        base + kSampR5ProcessInputRva);

    setCursorMode(game,
                  owned ? kSampCursorLockCamAndControl : kSampCursorNone,
                  owned ? FALSE : TRUE);
    processInput(game);
}

void EnterCustomPause() {
    // CMenuManager::Process must remain live: it maintains GTA's front-end
    // pause semantics. We hide only its drawing and zero its input separately.
    skr::lobby::RestoreGtaMainMenu();
    *reinterpret_cast<uint8_t*>(kDontDrawFrontEnd) = 1;
    *reinterpret_cast<uint8_t*>(kActivateMenuNextFrame) = 0;
    if (!*reinterpret_cast<uint8_t*>(kMenuActive))
        *reinterpret_cast<uint8_t*>(kMenuActive) = 1;
    if (!*reinterpret_cast<bool*>(kUserPause))
        reinterpret_cast<TimerPauseFn>(kStartUserPause)();
    SetSampInputOwnership(true);
    ShowPauseCursor();
}

void LeaveCustomPause(bool resumeGameplay) {
    HidePauseCursor();
    SetSampInputOwnership(false);
    *reinterpret_cast<uint8_t*>(kDontDrawFrontEnd) = 0;
    if (!resumeGameplay) return;

    *reinterpret_cast<uint8_t*>(kActivateMenuNextFrame) = 0;
    *reinterpret_cast<uint8_t*>(kMenuActive) = 0;
    if (*reinterpret_cast<bool*>(kUserPause))
        reinterpret_cast<TimerPauseFn>(kEndUserPause)();
}

// ── State transitions ───────────────────────────────────────────────
void TransitionTo(PauseState newState) {
    PauseState old = g_state;
    if (old == PauseState::PauseOpen && newState != PauseState::PauseOpen)
        controls::CancelCapture();
    g_state = newState;
    Log("state transition: %d -> %d", static_cast<int>(old), static_cast<int>(newState));

    // Suspend the phone when the pause menu opens; it never draws over the pause.
    if (newState == PauseState::PauseOpen)
        skr::phone::input::GetController().Suspend();

    ClearNav();
    if (newState == PauseState::PauseOpen)
        EnterCustomPause();
    else if (old == PauseState::PauseOpen)
        LeaveCustomPause(newState == PauseState::Gameplay);
}

// ── WndProc Hook ────────────────────────────────────────────────────
LRESULT CALLBACK HookedWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    return ProcessMessage(msg, wParam, lParam) ? 0
                                               : CallWindowProcA(g_origWndProc, hwnd, msg, wParam, lParam);
}

bool HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // Binding capture owns input before settings requests, ESC, ImGui, and
    // custom keyboard navigation. This prevents a captured key from also
    // activating a setting, switching tabs, or reaching GTA/SA-MP.
    if (g_state == PauseState::PauseOpen) {
        const bool captureAltTab = controls::IsCapturing() && msg == WM_SYSKEYDOWN &&
                                   wParam == VK_TAB &&
                                   (static_cast<uintptr_t>(lParam) & (1u << 29)) != 0;
        if (controls::ProcessMessage(msg, wParam, lParam)) return true;
        if (captureAltTab) return false;
    }

    if (settings::ProcessMessage(msg, wParam, lParam))
        return true;

    // Native Runtime API confirm dialog (Y / Enter = confirm, N / ESC = cancel).
    // Gameplay only, and before the ESC handling below so ESC answers the dialog
    // instead of opening the pause menu. Other keys are never consumed.
    if (g_state == PauseState::Gameplay &&
        skr::native_api::OnWndProc(msg, static_cast<uintptr_t>(wParam), static_cast<intptr_t>(lParam)))
        return true;

    // Phone input — during gameplay only. The phone never opens over the pause
    // menu (it suspends when the pause opens and vice versa). It sits after the
    // native API confirm so ESC answers that dialog first, and before the ESC
    // handler so ESC navigates the phone instead of opening the pause menu.
    if (g_state == PauseState::Gameplay &&
        skr::phone::input::ProcessMessage(msg, static_cast<uintptr_t>(wParam),
                                          static_cast<intptr_t>(lParam),
                                          GetTickCount64()))
        return true;

    // Update mouse state (always tracked for UI)
    switch (msg) {
        case WM_MOUSEMOVE:
            g_mouse.x = static_cast<short>(LOWORD(lParam));
            g_mouse.y = static_cast<short>(HIWORD(lParam));
            break;
        case WM_LBUTTONDOWN:
            g_mouse.leftDown = true;
            g_mouse.leftClicked = true;
            break;
        case WM_LBUTTONUP:
            g_mouse.leftDown = false;
            break;
        case WM_RBUTTONDOWN:
            g_mouse.rightClicked = true;
            break;
    }

    // ESC key handling (before ImGui, since ESC controls state)
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && wParam == VK_ESCAPE) {
        if ((static_cast<uint32_t>(lParam) & (1u << 30)) != 0) return true;
        DWORD now = GetTickCount();

        if (now - g_lastEscTime < kEscDebounceMs) {
            return true;  // Debounce — consume
        }
        g_lastEscTime = now;

        switch (g_state) {
            case PauseState::Gameplay:
                TransitionTo(PauseState::PauseOpen);
                return true;

            case PauseState::PauseOpen:
                // ESC closes an open confirmation dialog first.
                if (renderer::HandleBack()) return true;
                actions::Resume();
                return true;

            case PauseState::NativeSubmenu:
                // Let GTA handle ESC for its native submenu.
                // actions::Tick() detects when it closes.
                return false;
        }
    }

    if ((msg == WM_KEYUP || msg == WM_SYSKEYUP || msg == WM_CHAR) && wParam == VK_ESCAPE &&
        g_state != PauseState::NativeSubmenu)
        return true;

    // Forward messages to ImGui when pause menu is open.
    // This is critical — without it ImGui never receives mouse events,
    // so hover states, clicks on buttons, and scroll don't work.
    if (g_state == PauseState::PauseOpen) {
        if (msg == WM_SETFOCUS) {
            SetSampInputOwnership(true);
            ShowPauseCursor();
        }
        if (msg == WM_SETCURSOR) {
            SetCursor(LoadCursor(nullptr, IDC_ARROW));
            return true;
        }

        // Handle navigation and activation before ImGui. The backend may mark
        // ENTER as handled even though these custom-drawn buttons are not
        // native ImGui widgets.
        if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
            const bool repeated = (static_cast<uint32_t>(lParam) & (1u << 30)) != 0;
            // Q/E switch main tabs. Arrow keys move/adjust the focused
            // control (auto-repeat allowed); Enter/Space activate it.
            switch (wParam) {
                case 'Q':
                    if (!repeated && !renderer::IsModalOpen() && g_activeTab > PauseTab::Map)
                        g_activeTab = static_cast<PauseTab>(static_cast<int>(g_activeTab) - 1);
                    return true;
                case 'E':
                    if (!repeated && !renderer::IsModalOpen() && g_activeTab < static_cast<PauseTab>(static_cast<int>(PauseTab::Count) - 1))
                        g_activeTab = static_cast<PauseTab>(static_cast<int>(g_activeTab) + 1);
                    return true;
                case VK_UP:    PushNav(NavEvent::Up);    return true;
                case VK_DOWN:  PushNav(NavEvent::Down);  return true;
                case VK_LEFT:  PushNav(NavEvent::Left);  return true;
                case VK_RIGHT: PushNav(NavEvent::Right); return true;
                case VK_RETURN:
                case VK_SPACE:
                    if (!repeated) PushNav(NavEvent::Activate);
                    return true;
            }
        }

        // Forward to ImGui's Win32 handler first
        if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
            return true;  // ImGui consumed it

        switch (msg) {
            case WM_KEYDOWN:
            case WM_KEYUP:
            case WM_CHAR:
            case WM_LBUTTONDOWN:
            case WM_LBUTTONUP:
            case WM_RBUTTONDOWN:
            case WM_RBUTTONUP:
            case WM_MBUTTONDOWN:
            case WM_MBUTTONUP:
            case WM_MOUSEWHEEL:
            case WM_MOUSEHWHEEL:
                return true;  // Consume — don't let GTA see game input

            case WM_MOUSEMOVE:
            case WM_INPUT:
                return true;  // Consume — we track it above
        }
    }

    return false;
}

} // namespace

bool Initialize(HWND gtaWindow) {
    if (g_initialized) return true;
    if (!gtaWindow || !IsWindow(gtaWindow)) return false;

    g_window = gtaWindow;
    g_origWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrA(gtaWindow, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(&HookedWndProc)));

    if (!g_origWndProc) {
        Log("Initialize failed: SetWindowLongPtr returned null (err=%lu)", GetLastError());
        return false;
    }

    g_state = PauseState::Gameplay;
    g_activeTab = PauseTab::Game;
    settings::Initialize(gtaWindow);
    InstallDirectInputOwnership();
    g_initialized = true;
    Log("Initialize: WndProc hooked (orig=0x%p)", g_origWndProc);
    return true;
}

void Shutdown() {
    if (!g_initialized) return;

    if (g_state == PauseState::PauseOpen)
        LeaveCustomPause(true);
    controls::CancelCapture();
    settings::Shutdown();
    RemoveDirectInputOwnership();

    if (g_window && g_origWndProc) {
        const auto current = reinterpret_cast<WNDPROC>(GetWindowLongPtrA(g_window, GWLP_WNDPROC));
        if (current == &HookedWndProc) {
            SetWindowLongPtrA(g_window, GWLP_WNDPROC,
                              reinterpret_cast<LONG_PTR>(g_origWndProc));
            Log("Shutdown: WndProc restored");
        } else {
            Log("Shutdown: WndProc changed by another owner; not overwritten");
        }
    }

    g_origWndProc = nullptr;
    g_window = nullptr;
    g_state = PauseState::Gameplay;
    g_initialized = false;
}

bool ProcessMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    return HandleMessage(g_window, msg, wParam, lParam);
}

void Tick() {
    if (g_state == PauseState::PauseOpen) {
        // Mirror omp-cef's FocusManager behavior: SA-MP may update its input
        // state during the frame, so ownership is asserted while focused.
        SetSampInputOwnership(true);
    }
}

PauseState GetState() { return g_state; }
bool OwnsInput() { return g_state == PauseState::PauseOpen; }
void SetState(PauseState state) { TransitionTo(state); }

PauseTab GetActiveTab() { return g_activeTab; }
void SetActiveTab(PauseTab tab) { g_activeTab = tab; }

const MouseState& GetMouse() {
    return g_mouse;
}

bool IsSampR5Loaded() {
    return IsPinnedSampR5(GetModuleHandleW(L"samp.dll"));
}

NavEvent PopNavEvent() {
    if (g_navCount <= 0) return NavEvent::None;
    const NavEvent ev = g_navQueue[g_navHead];
    g_navHead = (g_navHead + 1) % kNavQueueSize;
    --g_navCount;
    return ev;
}

} // namespace skr::native_pause::input
