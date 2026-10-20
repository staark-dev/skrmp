#pragma once
#include <windows.h>
#include <d3d9.h>
#include <cstdint>
namespace skr::launch_cover {
void Configure(uint32_t frame, uint32_t takeover, uint32_t ready);
// Render thread only; readiness is visual progress, never server authorization.
bool Draw(IDirect3DDevice9* device, bool worldConnected);
void Presented(bool drewCover, HRESULT result);
void BeforeReset();
// Called by the lobby when gameplay is confirmed; unblocks the cover
// even when CEF visual-ready cannot be satisfied (e.g. HUD browser 101
// was never created or ResourcesReadyForHandoff stays false).
void MarkGameplayConfirmed();
}
