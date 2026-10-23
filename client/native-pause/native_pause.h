#pragma once
// ═══════════════════════════════════════════════════════════════════════
// SKR Native Pause Menu — Task #22
// Replaces the CEF-based Escape Menu with a fully native C++/DirectX 9
// pause menu rendered via Dear ImGui, styled to match GTA San Andreas.
// ═══════════════════════════════════════════════════════════════════════

#include <cstdint>

struct IDirect3DDevice9;

namespace skr::native_pause {

// ── Lifecycle ───────────────────────────────────────────────────────
// Called once after D3D9 hooks are installed and the device is known.
bool Initialize();

// Called during SkrShutdown (before D3D9 hooks are removed).
void Shutdown();

// ── Per-frame ───────────────────────────────────────────────────────
// Called from HookedEndScene — renders the pause overlay when open.
void OnEndScene(IDirect3DDevice9* device);

// Called from HookedReset — releases/recreates device-dependent resources.
void OnBeforeReset();
void OnAfterReset(IDirect3DDevice9* device);

// ── Input ───────────────────────────────────────────────────────────
// Win32 message filter — returns true if the message was consumed.
// Must be called from the GTA window's message loop (WndProc hook).
bool OnWndProc(unsigned int msg, uintptr_t wParam, intptr_t lParam);

// ── State queries ───────────────────────────────────────────────────
bool IsOpen();
bool IsInitialized();

} // namespace skr::native_pause
