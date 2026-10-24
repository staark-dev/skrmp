#pragma once
#include <d3d9.h>
namespace skr::render::fps {
// These functions run only on GTA's D3D9 render thread.
void Initialize();
// Re-reads fps.ini (show counter / cap) so changes apply without a restart.
void ReloadPreferences();
void BeforeReset();
void Shutdown();
void Draw(IDirect3DDevice9* device);
void Presented(HRESULT result, bool isInWorld);
void Trace(const char* message);
}
