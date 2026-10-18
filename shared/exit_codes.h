#pragma once
#include <cstdint>

namespace skr {

// GTA process exit codes interpreted by the launcher.
// Normal exit (0) ends the session; kExitRestartLobby causes the launcher
// to re-launch GTA with the lobby (like FiveM's "back to main menu").
constexpr uint32_t kExitRestartLobby = 42;

} // namespace skr
