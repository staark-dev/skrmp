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

// Per-frame render through the single ImGui owner. drawPauseMenu=false renders
// only the gameplay overlay (GPS navigation panel), with no input capture.
void Render(IDirect3DDevice9* device, bool drawPauseMenu = true);

// Device lost/reset handling.
void OnBeforeReset();
void OnAfterReset(IDirect3DDevice9* device);

// Returns true if ImGui is initialized and ready.
bool IsReady();

// Confirmation dialogs. HandleBack() closes an open dialog (ESC) and returns
// true if it did; IsModalOpen() lets input suppress tab switching.
bool HandleBack();
bool IsModalOpen();

} // namespace skr::native_pause::renderer
