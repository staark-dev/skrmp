#pragma once
// ═══════════════════════════════════════════════════════════════════════
// SKR Native Pause Menu — Renderer
// Dear ImGui (Win32/DX9 backend) with custom GTA SA styling.
// Renders the full-screen pause overlay, tab bar, and content panels.
// ═══════════════════════════════════════════════════════════════════════

#include <d3d9.h>

namespace skr::native_pause::renderer {

// Initialize Dear ImGui context and DX9/Win32 backends.
bool Initialize(IDirect3DDevice9* device, HWND window);

// Shutdown and clean up ImGui context.
void Shutdown();

// Per-frame render. Only call when the pause menu is open.
void Render(IDirect3DDevice9* device);

// Device lost/reset handling.
void OnBeforeReset();
void OnAfterReset(IDirect3DDevice9* device);

// Returns true if ImGui is initialized and ready.
bool IsReady();

} // namespace skr::native_pause::renderer
