#pragma once
#include <cstdint>

struct IDirect3DDevice9;

namespace skr::lobby {

// Phase 1 lobby orchestrator:
//   - Patches CMenuManager so the vanilla front-end never runs.
//   - Creates the SKR lobby CEF browser (BrowserId::kMainMenu) full-frame when
//     the SKR intro finishes and the CEF plugin is ready.
//   - Subscribes to "skr:lobby:*" events for settings get/save, play (stubbed
//     in Phase 1), and quit.
//   - Draws a full-screen opaque black quad after the game's EndScene so any
//     residual GTA menu pixels do not show through under the CEF overlay.
void Initialize();
void Shutdown();

// Called once per EndScene by d3d9_hook after the intro reports IsFinished().
// `device` is the device the game's EndScene received.
void OnEndSceneAfterIntro(IDirect3DDevice9* device);

// Called from cef_samp_mainloop (CEF plugin thread, once per frame).
// Idempotent: creates the browser only once CEF reports ready.
void OnCefTick();

// Forwards SA-MP's native CNetGame state to the lobby's connecting overlay.
// Called on the game thread every tick; deduplicated.
void OnNetGameState(int sampState);

// Server session info (RPC 241, shared/protocol/session_info.h). Game thread.
void OnSessionInfo(uint8_t mode);

// True once the server sent RPC 241, i.e. it runs the SKR Gate. SKR-only
// behaviour (RPC 240, CEF HUD in place of the native one, chat panel) is
// limited to such sessions; other servers get plain SA-MP.
bool IsSkrSession();

// Called on the game thread while connected with a ped in the world. Hands
// over to the server: right away for SessionMode::None, once the server's own
// CEF page takes focus for SessionMode::CefIntro, after a short grace period
// when the server sent no session info (non-SKR server).
void OnInWorldTick();

// Hides the lobby and returns input to GTA once the player is in the server's
// world. Called on the game thread; idempotent.
void OnGameplayConfirmed(const char* reason);

// True once the lobby browser has been created and shown.
bool IsLobbyActive();

// True when the lobby module is enabled for this run (SKR\lobby.on exists).
// When false, OnEndSceneAfterIntro / OnCefTick are no-ops and the caller
// should fall back to vanilla-menu behavior.
bool IsEnabled();

} // namespace skr::lobby
