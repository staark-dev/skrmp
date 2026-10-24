#pragma once
#include <windows.h>
#include <d3d9.h>
#include <cstdint>
namespace skr::launch_cover {
// Launcher-owned cover event handles (0 = absent). Readiness milestones
// (SAMP_READY, GAMEPLAY_CONFIRMED, VISUAL_READY) go through launch_signals.
void Configure(uint32_t frame, uint32_t takeover);
// Render thread only; readiness is visual progress, never server authorization.
// Returns true when the cover was drawn this frame. On the completion frame
// nothing is drawn and visual-ready is signalled once that frame is presented.
bool Draw(IDirect3DDevice9* device, bool worldConnected);
void Presented(bool drewCover, HRESULT result);
void BeforeReset();
// Called by the lobby when gameplay is confirmed; unblocks the cover
// even when CEF visual-ready cannot be satisfied (e.g. HUD browser 101
// was never created or ResourcesReadyForHandoff stays false). Also signals
// GAMEPLAY_CONFIRMED to the launcher (once).
void MarkGameplayConfirmed();
// Called from game_state::Tick() when CNetGame is first detected.
// Triggers cover dismissal before the "Started" chat message renders, and
// signals SAMP_READY to the launcher (once).
void MarkNetGameReady();
}
