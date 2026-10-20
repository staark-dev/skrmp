#pragma once

namespace skr::game_state {
void ConfigureEnhancedDiscovery(bool advertised);
bool IsEnhancedNegotiationPending();

// Redirects the CGame::Process call inside Idle (0x53E981) so Tick runs once per
// game frame on the GTA thread. Call after SA-MP has installed its own hooks.
bool InstallProcessHook();

// Hooks CHud::Draw so CRadar::DrawMap runs in RenderWare's 2D pipeline
// instead of EndScene (where RW's render-state cache is stale).
bool InstallHudDrawHook();

} // namespace skr::game_state
