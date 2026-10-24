#pragma once
// ═══════════════════════════════════════════════════════════════════════
// SKR Native Runtime API — client side (TASK API-001)
// Spec: docs/protocol/SKR_NATIVE_API.md. Codec: shared/protocol/native_api.h.
//
// Lifecycle (all on the GTA thread):
//   game_state OnSessionInfoRpc: Enhanced confirmed -> OnEnhancedConfirmed(token)
//   Tick() sends Hello, waits for Welcome (5 s, then gives up: plain/older server)
//   OnServerMessage() handles Welcome and Calls (envelope steps 6 and 7)
//   Draw() renders notifications / the confirm dialog in GTA's 2D HUD pass
//   OnWndProc() answers the dialog (pause_input, gameplay state only)
//   Reset() on disconnect: session id, dialogs and HUD override are dropped
//
// Nothing here runs unless the server is an SKR Runtime that completed the
// Enhanced handshake, so ordinary SA-MP/open.mp servers are unaffected.
// ═══════════════════════════════════════════════════════════════════════

#include <cstddef>
#include <cstdint>

namespace skr::native_api {

void OnEnhancedConfirmed(uint32_t enhancedToken);
void OnServerMessage(const uint8_t* data, size_t len);
void Tick();
void Reset();
void Draw();      // legacy GTA 2D pass (confirm dialog only when ImGui unavailable)
bool OnWndProc(unsigned int msg, uintptr_t wParam, intptr_t lParam);   // true = consumed

bool IsReady();

} // namespace skr::native_api
