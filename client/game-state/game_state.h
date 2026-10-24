#pragma once
#include <cstddef>
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

// Resolves the GTA SA navigation zone for a world position.
// city = largest containing zone with z2==900 (Los Santos, San Fierro, etc.).
// area = smallest containing info zone (Rodeo, Market, etc.).
void ResolveZones(float x, float y, float z, char* city, size_t cityLen, char* area, size_t areaLen);

// Sends raw bytes as SKR RPC 240 (client -> SKR Gate), reliable ordered, like the
// Enhanced handshake. Only for SKR Enhanced envelope messages (first byte 0xE0).
// GTA thread only. False when SA-MP is not connected or the send failed.
bool SendEnhancedEnvelope(const uint8_t* data, size_t len);

} // namespace skr::game_state
