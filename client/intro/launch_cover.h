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
}
