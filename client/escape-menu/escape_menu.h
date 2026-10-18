#pragma once
// SKR custom ESC menu — replaces the GTA SA pause menu on SKR servers.
// Uses omp-cef's EscapeMenuController in Custom mode: ESC toggles the
// "cef:escape_menu" event to a dedicated CEF browser page.

#include <cstdint>

namespace skr::escape_menu {

// Call once after CEF bridge is ready AND gameplay is confirmed on an SKR server.
// Sets omp-cef escape mode to Custom and creates the menu browser.
void Enable();

// Reverts to native GTA menu and destroys the browser.
void Disable();

// Per-frame tick — manages browser lifecycle in response to escape_menu events.
void Tick();

bool IsEnabled();
bool IsOpen();

} // namespace skr::escape_menu
