#pragma once

namespace skr::game_state {

// Redirects the CGame::Process call inside Idle (0x53E981) so Tick runs once per
// game frame on the GTA thread. Call after SA-MP has installed its own hooks.
bool InstallProcessHook();

} // namespace skr::game_state
