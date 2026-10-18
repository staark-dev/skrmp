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

// True once the lobby browser has been created and shown.
bool IsLobbyActive();

// True when the lobby module is enabled for this run (SKR\lobby.on exists).
// When false, OnEndSceneAfterIntro / OnCefTick are no-ops and the caller
// should fall back to vanilla-menu behavior.
bool IsEnabled();

} // namespace skr::lobby
