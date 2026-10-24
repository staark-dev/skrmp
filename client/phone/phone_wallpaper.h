#pragma once
// PHONE-M4F — SKR Street Phone: wallpaper texture.
//
// Loads <dir of SKR.Client.dll>\SKR\phone\wallpaper.bmp (staged by CMake from
// assets/bg.bmp) into ONE D3DPOOL_MANAGED texture, so a device Reset needs no
// work; a replaced device (SA-MP world handoff) drops it and the next frame
// loads it again on the new device. Uncompressed 24/32-bit BMP only, parsed
// here (no WIC/COM on the render thread). Missing or bad file: the renderer
// keeps its procedural wallpaper. Each device is tried once, never per frame.

struct IDirect3DDevice9;

namespace skr::phone::wallpaper {

// Loads on first call for `device`; afterwards a no-op. Publishes the texture
// to the renderer (SetWallpaperTexture). Render thread only.
void Ensure(IDirect3DDevice9* device);

// Explicit file (preview harness). Returns true when a texture is published.
bool LoadFile(IDirect3DDevice9* device, const wchar_t* path);

// Device replaced or shutdown: releases the texture and clears the renderer's copy.
void Release();

} // namespace skr::phone::wallpaper
