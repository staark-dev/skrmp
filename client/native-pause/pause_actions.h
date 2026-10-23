#pragma once
// ═══════════════════════════════════════════════════════════════════════
// SKR Native Pause Menu — Actions
// Handles Resume, Exit, native MAP/SETTINGS submenu integration,
// disconnect. Reuses escape_menu patterns for CMenuManager interaction.
// ═══════════════════════════════════════════════════════════════════════

namespace skr::native_pause::actions {

// Resume gameplay — close the pause menu and restore GTA state.
void Resume();

// Open GTA's native MAP submenu (page 5).
void OpenNativeMap();

// Open GTA's native SETTINGS submenu (page 33).
void OpenNativeSettings();

// Disconnect from the current server.
void Disconnect();

// Exit the game entirely.
void ExitGame();

// Per-frame tick — monitors GTA native submenu lifecycle.
// When a native submenu (MAP/SETTINGS) closes, transitions back to PauseOpen.
void Tick();

} // namespace skr::native_pause::actions
