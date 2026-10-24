#include "render-d3d9/fps_engine.h"
#include "performance/fps_settings.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include "ipc/handoff_trace.h"

namespace skr::render::fps {
namespace {
using Microsoft::WRL::ComPtr;
struct Vertex { float x, y, z, rhw; DWORD color; };
constexpr DWORD kFVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;
performance::FpsSettings g_preferences;
LARGE_INTEGER g_frequency{};
LARGE_INTEGER g_windowStart{};
LARGE_INTEGER g_lastPaced{};
unsigned g_presentCount = 0;
unsigned g_displayFps = 0;
ComPtr<IDirect3DStateBlock9> g_savedState;

const uint8_t* Glyph(char c) {
    // 3x5 hand-built font, no D3DX/CEF/texture dependency.
    static const uint8_t digits[10][5] = {
        {7,5,5,5,7}, {2,6,2,2,7}, {7,1,7,4,7}, {7,1,7,1,7},
        {5,5,7,1,1}, {7,4,7,1,7}, {7,4,7,5,7}, {7,1,1,1,1},
        {7,5,7,5,7}, {7,5,7,1,7}
    };
    static const uint8_t f[5] = {7,4,6,4,4};
    static const uint8_t p[5] = {7,5,7,4,4};
    static const uint8_t s[5] = {7,4,7,1,7};
    static const uint8_t space[5] = {0,0,0,0,0};
    static const uint8_t dash[5] = {0,0,7,0,0};
    if (c >= '0' && c <= '9') return digits[c - '0'];
    if (c == 'F') return f;
    if (c == 'P') return p;
    if (c == 'S') return s;
    if (c == '-') return dash;
    return space;
}
void AddRect(std::vector<Vertex>& vertices, float x, float y, float w, float h, DWORD color) {
    // D3D9 half-pixel alignment for pixel-perfect 2D rendering.
    x -= 0.5f; y -= 0.5f;
    vertices.push_back({x,y,0,1,color});
    vertices.push_back({x+w,y,0,1,color});
    vertices.push_back({x,y+h,0,1,color});
    vertices.push_back({x+w,y,0,1,color});
    vertices.push_back({x+w,y+h,0,1,color});
    vertices.push_back({x,y+h,0,1,color});
}
void Pace(unsigned cap) {
    if (!cap || !g_frequency.QuadPart) { g_lastPaced.QuadPart = 0; return; }
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    const LONGLONG interval = g_frequency.QuadPart / static_cast<LONGLONG>(cap);
    if (g_lastPaced.QuadPart > 0 && interval > 0) {
        const LONGLONG target = g_lastPaced.QuadPart + interval;
        if (target > now.QuadPart) {
            const LONGLONG remainingMs = (target - now.QuadPart) * 1000 / g_frequency.QuadPart;
            // Never spin the GTA thread: coarse cooperative cap, not hard real-time.
            if (remainingMs >= 2) Sleep(static_cast<DWORD>(remainingMs - 1));
            QueryPerformanceCounter(&now);
        }
    }
    g_lastPaced = now;
}
} // namespace
void Trace(const char* message) { skr::ipc::TraceHandoff(message); }

void Initialize() {
    g_preferences = performance::LoadFpsSettings();
    QueryPerformanceFrequency(&g_frequency);
    g_windowStart = {};
    g_lastPaced = {};
    g_presentCount = g_displayFps = 0;
    char message[128]{};
    sprintf_s(message, "fps initialized show=%u cap=%u qpc=%lld",
              g_preferences.showCounter ? 1u : 0u, g_preferences.cap, g_frequency.QuadPart);
    Trace(message);
}
void ReloadPreferences() {
    g_preferences = performance::LoadFpsSettings();
    g_lastPaced = {};
    char message[96]{};
    sprintf_s(message, "fps preferences reloaded show=%u cap=%u",
              g_preferences.showCounter ? 1u : 0u, g_preferences.cap);
    Trace(message);
}
void BeforeReset() {
    g_savedState.Reset(); g_windowStart = {}; g_lastPaced = {}; g_presentCount = 0;
    Trace("fps reset resources released; sampling restarted");
}
void Shutdown() { g_savedState.Reset(); }

void Presented(HRESULT result, bool isInWorld) {
    if (FAILED(result) || !isInWorld || !g_frequency.QuadPart) {
        g_lastPaced.QuadPart = 0;
        g_windowStart = {}; g_presentCount = 0;
        return;
    }
    // The selected cap works only if GTA/SA-MP/vsync are not already imposing a lower one.
    Pace(g_preferences.cap);
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    if (!g_windowStart.QuadPart) { g_windowStart = now; g_presentCount = 0; return; }
    ++g_presentCount;
    const LONGLONG elapsed = now.QuadPart - g_windowStart.QuadPart;
    if (elapsed >= g_frequency.QuadPart / 2) {
        const double seconds = static_cast<double>(elapsed) / static_cast<double>(g_frequency.QuadPart);
        g_displayFps = static_cast<unsigned>(g_presentCount / seconds + 0.5);
        g_presentCount = 0;
        g_windowStart = now;
        static ULONGLONG lastLog = 0;
        if (GetTickCount64() - lastLog >= 5000) {
            lastLog = GetTickCount64();
            char message[96]{};
            sprintf_s(message, "fps measured=%u windowSeconds=%.3f", g_displayFps, seconds);
            Trace(message);
        }
    }
}

void Draw(IDirect3DDevice9* device) {
    if (!g_preferences.showCounter || !device) return;
    static ULONGLONG lastLog = 0;
    const bool log = GetTickCount64() - lastLog >= 5000;
    if (log) lastLog = GetTickCount64();
    auto report = [log](const char* stage, HRESULT hr) {
        if (!log) return;
        char message[128]{}; sprintf_s(message, "fps %s hr=0x%08lX", stage, hr); Trace(message);
    };

    // CEF and GTA share this D3D9 device. State blocks include pipeline/viewport
    // state but DO NOT save render targets or depth-stencil surfaces. In
    // particular, drawing into the CEF texture instead of GTA's back buffer can
    // corrupt the next HUD frame. Save these surfaces explicitly.
    ComPtr<IDirect3DSurface9> previousTarget;
    ComPtr<IDirect3DSurface9> backBuffer;
    ComPtr<IDirect3DSurface9> previousDepth;
    if (FAILED(device->GetRenderTarget(0, previousTarget.GetAddressOf())) ||
        FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, backBuffer.GetAddressOf())))
        return;

    // A fresh state block avoids holding references to CEF textures between
    // frames. CreateStateBlock captures the current states automatically.
    // The shared pointer is also released before Reset/Shutdown.
    g_savedState.Reset();
    if (FAILED(device->CreateStateBlock(D3DSBT_ALL, g_savedState.GetAddressOf()))) return;
    if (FAILED(g_savedState->Capture())) { g_savedState.Reset(); return; }

    const bool redirect = previousTarget.Get() != backBuffer.Get();
    bool targetChanged = false;
    bool depthDetached = false;
    auto restore = [&]() {
        // SetRenderTarget can reset the viewport; restore targets FIRST,
        // then re-apply the full captured pipeline/viewport state.
        if (targetChanged) device->SetRenderTarget(0, previousTarget.Get());
        if (depthDetached) device->SetDepthStencilSurface(previousDepth.Get());
        if (g_savedState) {
            g_savedState->Apply();
            g_savedState.Reset();
        }
    };

    if (redirect) {
        if (SUCCEEDED(device->GetDepthStencilSurface(previousDepth.GetAddressOf())) && previousDepth) {
            if (FAILED(device->SetDepthStencilSurface(nullptr))) { restore(); return; }
            depthDetached = true;
        }
        if (FAILED(device->SetRenderTarget(0, backBuffer.Get()))) { restore(); return; }
        targetChanged = true;
    }

    D3DSURFACE_DESC desc{};
    if (FAILED(backBuffer->GetDesc(&desc))) { restore(); return; }
    D3DVIEWPORT9 vp{0, 0, desc.Width, desc.Height, 0.f, 1.f};
    if (FAILED(device->SetViewport(&vp))) { restore(); return; }
    // Present runs outside GTA's scene. DrawPrimitiveUP requires an active scene.
    // Do not issue EndScene if BeginScene failed (including an unexpected nested scene).
    const HRESULT begin = device->BeginScene();
    report("BeginScene", begin);
    if (FAILED(begin)) { restore(); return; }
    std::vector<Vertex> mesh;
    mesh.reserve(850);
    char text[24]{};
    sprintf_s(text, "FPS %u", (std::min)(g_displayFps, 9999u));
    const size_t length = strlen(text);
    constexpr float pixel = 2.0f, advance = 9.0f;
    const float width = 22.0f + static_cast<float>(length) * advance;
    const float x = static_cast<float>(vp.X) + 12.0f;
    const float y = static_cast<float>(vp.Y) + 12.0f;
    AddRect(mesh, x, y, width, 24.0f, 0xB5000000);
    for (size_t i = 0; i < length; ++i) {
        const uint8_t* g = Glyph(text[i]);
        DWORD color = i <= 2 ? 0xFFEEEEEE : 0xFF8FD674;
        for (int row = 0; row < 5; ++row) {
            for (int column = 0; column < 3; ++column) {
                if (g[row] & (1u << (2 - column)))
                    AddRect(mesh, x + 10.0f + static_cast<float>(i) * advance + column * pixel,
                            y + 7.0f + row * pixel, pixel, pixel, color);
            }
        }
    }
    device->SetVertexShader(nullptr);
    device->SetPixelShader(nullptr);
    device->SetFVF(kFVF);
    device->SetTexture(0, nullptr);
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_FOGENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    device->SetRenderState(D3DRS_COLORWRITEENABLE, 15);
    device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    const HRESULT draw = device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, static_cast<UINT>(mesh.size() / 3), mesh.data(), sizeof(Vertex));
    report("DrawPrimitiveUP", draw);
    report("EndScene", device->EndScene());
    restore();
}

} // namespace skr::render::fps
