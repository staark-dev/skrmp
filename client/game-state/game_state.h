#pragma once
#include <cstdint>

namespace skr::game_state {

// ── Client connection mode (task-spec §3 state machine) ─────────────
// STANDARD     – connected to a vanilla SA-MP / open.mp server.
// NEGOTIATING  – enhanced handshake in progress (Hello sent, awaiting Offer/Confirm).
// ENHANCED     – SKR Runtime confirmed; enhanced RPCs and features unlocked.
enum class ClientConnectionMode : uint8_t {
    Standard    = 0,
    Negotiating = 1,
    Enhanced    = 2,
};

// Returns the current connection mode.  Thread-safe (reads an atomic).
ClientConnectionMode GetConnectionMode();

void ConfigureEnhancedDiscovery(bool advertised);
bool IsEnhancedNegotiationPending();

// Redirects the CGame::Process call inside Idle (0x53E981) so Tick runs once per
// game frame on the GTA thread. Call after SA-MP has installed its own hooks.
bool InstallProcessHook();

// Hooks CHud::Draw so CRadar::DrawMap runs in RenderWare's 2D pipeline
// instead of EndScene (where RW's render-state cache is stale).
bool InstallHudDrawHook();

} // namespace skr::game_state
