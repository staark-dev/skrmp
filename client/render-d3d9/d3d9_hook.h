#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include "bootstrap/init_state.h"

namespace skr::render {

// Indices into the IDirect3DDevice9 vtable
constexpr int kVtableEndScene = 42;
constexpr int kVtableReset    = 16;

using EndSceneFn = HRESULT(WINAPI*)(IDirect3DDevice9*);
using ResetFn    = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

bool InstallD3D9Hooks();
skr::bootstrap::ComponentState GetD3D9HookState() noexcept;
void RemoveD3D9Hooks();

} // namespace skr::render
