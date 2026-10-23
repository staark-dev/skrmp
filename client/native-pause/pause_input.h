#pragma once
// ═══════════════════════════════════════════════════════════════════════
// SKR Native Pause Menu — Input Handler
// ESC key interception with state machine:
//   GAMEPLAY → SKR_PAUSE_OPEN → GTA_NATIVE_SUBMENU → back
// ═══════════════════════════════════════════════════════════════════════

#include <cstdint>
#include <windows.h>

namespace skr::native_pause::input {

// Pause menu states
enum class PauseState : uint8_t {
    Gameplay        = 0,   // Normal gameplay, pause menu hidden
    PauseOpen       = 1,   // SKR pause menu is displayed
    NativeSubmenu   = 2,   // GTA native submenu (MAP or SETTINGS) is active
};

// Initialize the input system — installs the WndProc hook on the GTA window.
bool Initialize(HWND gtaWindow);

// Shutdown — removes the WndProc hook.
void Shutdown();

// Process a Win32 message. Returns true if consumed (should not propagate).
bool ProcessMessage(UINT msg, WPARAM wParam, LPARAM lParam);

// Reassert SA-MP cursor/input ownership while the custom pause is open.
// SA-MP applies the same ownership update every frame for focused CEF views.
void Tick();

// Current state
PauseState GetState();

// True only while the SKR pause surface owns gameplay input. Native GTA
// submenus deliberately return ownership to GTA.
bool OwnsInput();

// Force a state transition (used by pause_actions for submenu navigation).
void SetState(PauseState state);

// Tab navigation
enum class PauseTab : uint8_t {
    Map      = 0,
    Game     = 1,
    Settings = 2,
    Servers  = 3,
    About    = 4,
    Count    = 5
};

PauseTab GetActiveTab();
void     SetActiveTab(PauseTab tab);

// Mouse state for UI interaction
struct MouseState {
    int x, y;
    bool leftDown;
    bool leftClicked;   // true for one frame on click
    bool rightClicked;
};
const MouseState& GetMouse();

} // namespace skr::native_pause::input
