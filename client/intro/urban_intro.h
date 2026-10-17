#pragma once
#include <d3d9.h>

namespace skr::intro {

// Urban SKR intro. OnFrame is fed by the existing SKRClient EndScene owner; once samp.dll
// is present the intro draws from SA-MP's Present proxy instead (see urban_intro.cpp).
// Every method except Shutdown/SetWorldReady must be called on the D3D9 device thread.
void OnFrame(IDirect3DDevice9* device);
// The player is in the world: leave the hold and run the final fade (picture + music).
void SetWorldReady();
// Ends the intro immediately.
void Skip();
void BeforeReset();
void AfterReset(HRESULT hr);
bool IsFinished();
void Shutdown();

} // namespace skr::intro
