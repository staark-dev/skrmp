#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <wrl/client.h>
#include <cstdio>
#include "render-d3d9/fps_engine.h"
using Microsoft::WRL::ComPtr;
int main() {
    HWND window = CreateWindowW(L"STATIC", L"SKR D3D9 validation", WS_POPUP,
                               0, 0, 320, 240, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ComPtr<IDirect3D9> d3d; d3d.Attach(Direct3DCreate9(D3D_SDK_VERSION));
    if (!window || !d3d) return 2;
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = window;
    pp.BackBufferWidth = 320; pp.BackBufferHeight = 240;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    ComPtr<IDirect3DDevice9> device;
    HRESULT hr = d3d->CreateDevice(0, D3DDEVTYPE_HAL, window,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, device.GetAddressOf());
    if (FAILED(hr)) { printf("D3D9 unavailable: %08lX\n", hr); DestroyWindow(window); return 2; }
    skr::render::fps::Initialize(); // Uses existing ShowCounter; disabled is reported as a failed pixel check.
    int failures = 0;
    auto check = [&](bool ok, const char* name) { printf("%s %s\n", ok ? "PASS" : "FAIL", name); failures += !ok; };
    for (int cycle = 0; cycle < 2; ++cycle) {
        device->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1, 0);
        ComPtr<IDirect3DSurface9> scratch, back, readback;
        device->CreateRenderTarget(64, 64, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, scratch.GetAddressOf(), nullptr);
        device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, back.GetAddressOf());
        device->SetRenderTarget(0, scratch.Get());
        D3DVIEWPORT9 viewport{2, 3, 40, 41, 0, 1};
        device->SetViewport(&viewport);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, 0);
        skr::render::fps::Draw(device.Get());
        ComPtr<IDirect3DSurface9> restored;
        device->GetRenderTarget(0, restored.GetAddressOf());
        check(restored.Get() == scratch.Get(), "render target restored");
        D3DVIEWPORT9 after{}; device->GetViewport(&after);
        check(after.X == 2 && after.Y == 3 && after.Width == 40 && after.Height == 41, "viewport restored");
        DWORD colorWrite = 99; device->GetRenderState(D3DRS_COLORWRITEENABLE, &colorWrite);
        check(colorWrite == 0, "pipeline state restored");
        device->CreateOffscreenPlainSurface(320, 240, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, readback.GetAddressOf(), nullptr);
        bool colored = false;
        if (readback && SUCCEEDED(device->GetRenderTargetData(back.Get(), readback.Get()))) {
            D3DLOCKED_RECT bits{};
            if (SUCCEEDED(readback->LockRect(&bits, nullptr, D3DLOCK_READONLY))) {
                for (int y = 12; y < 36; ++y) {
                    const auto* row = reinterpret_cast<const DWORD*>(static_cast<const char*>(bits.pBits) + y * bits.Pitch);
                    for (int x = 12; x < 110; ++x) colored |= (row[x] & 0x00FFFFFF) != 0;
                }
                readback->UnlockRect();
            }
        }
        check(colored, "FPS pixels rendered on back buffer");
        device->SetRenderTarget(0, back.Get());
        restored.Reset(); scratch.Reset(); back.Reset(); readback.Reset();
        skr::render::fps::BeforeReset();
        check(SUCCEEDED(device->Reset(&pp)), "device Reset succeeds");
    }
    skr::render::fps::Shutdown(); device.Reset(); d3d.Reset(); DestroyWindow(window);
    return failures ? 1 : 0;
}
