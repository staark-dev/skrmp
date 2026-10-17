#include "render-d3d9/d3d9_hook.h"
#include "render-d3d9/intro_renderer.h"
#include <atomic>

namespace skr::render {

namespace {

EndSceneFn g_origEndScene = nullptr;
ResetFn    g_origReset    = nullptr;
IntroRenderer g_intro;
std::atomic<bool> g_initialized{false};
LARGE_INTEGER g_lastTime{};
LARGE_INTEGER g_freq{};

bool PatchVtable(void** vtable, int index, void* hook, void** original) {
    DWORD oldProtect = 0;
    void* entry = &vtable[index];
    if (!VirtualProtect(entry, sizeof(void*), PAGE_READWRITE, &oldProtect))
        return false;
    *original = vtable[index];
    vtable[index] = hook;
    VirtualProtect(entry, sizeof(void*), oldProtect, &oldProtect);
    return true;
}

bool RestoreVtable(void** vtable, int index, void* original) {
    DWORD oldProtect = 0;
    void* entry = &vtable[index];
    if (!VirtualProtect(entry, sizeof(void*), PAGE_READWRITE, &oldProtect))
        return false;
    vtable[index] = original;
    VirtualProtect(entry, sizeof(void*), oldProtect, &oldProtect);
    return true;
}

HRESULT WINAPI HookedEndScene(IDirect3DDevice9* device) {
    if (!g_initialized.load()) {
        if (g_intro.CreateResources(device)) {
            QueryPerformanceFrequency(&g_freq);
            QueryPerformanceCounter(&g_lastTime);
            g_initialized.store(true);
        }
    }

    if (g_initialized.load() && !g_intro.IsDone()) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        float dt = static_cast<float>(now.QuadPart - g_lastTime.QuadPart)
                   / static_cast<float>(g_freq.QuadPart);
        g_lastTime = now;
        // Clamp delta to avoid jumps after alt-tab
        if (dt > 0.1f) dt = 0.1f;

        // Skip on any key press
        for (int vk = 0x01; vk <= 0xFE; ++vk) {
            if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON) continue;
            if (GetAsyncKeyState(vk) & 0x0001) {
                g_intro.Skip();
                break;
            }
        }

        g_intro.Draw(device, dt);
    }

    return g_origEndScene(device);
}

HRESULT WINAPI HookedReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* pp) {
    g_intro.BeforeReset();
    HRESULT hr = g_origReset(device, pp);
    if (SUCCEEDED(hr)) {
        g_intro.AfterReset(device);
        QueryPerformanceCounter(&g_lastTime);
    }
    return hr;
}

IDirect3DDevice9* CreateDummyDevice(IDirect3D9* d3d, HWND hwnd) {
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.hDeviceWindow = hwnd;

    IDirect3DDevice9* device = nullptr;
    HRESULT hr = d3d->CreateDevice(
        D3DADAPTER_DEFAULT, D3DDEVTYPE_NULLREF, hwnd,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_DISABLE_DRIVER_MANAGEMENT,
        &pp, &device);
    if (FAILED(hr)) return nullptr;
    return device;
}

} // anonymous namespace

bool InstallD3D9Hooks() {
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) return false;

    HWND hwnd = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPEDWINDOW,
                                0, 0, 1, 1, nullptr, nullptr, nullptr, nullptr);
    if (!hwnd) { d3d->Release(); return false; }

    IDirect3DDevice9* dummy = CreateDummyDevice(d3d, hwnd);
    if (!dummy) {
        DestroyWindow(hwnd);
        d3d->Release();
        return false;
    }

    void** vtable = *reinterpret_cast<void***>(dummy);
    bool ok = true;
    ok &= PatchVtable(vtable, kVtableEndScene,
                       reinterpret_cast<void*>(&HookedEndScene),
                       reinterpret_cast<void**>(&g_origEndScene));
    ok &= PatchVtable(vtable, kVtableReset,
                       reinterpret_cast<void*>(&HookedReset),
                       reinterpret_cast<void**>(&g_origReset));

    dummy->Release();
    DestroyWindow(hwnd);
    d3d->Release();
    return ok;
}

void RemoveD3D9Hooks() {
    g_intro.Shutdown();
    // Vtable restoration requires the live device vtable, which may differ
    // from the dummy. For MVP the DLL stays resident until game exit.
    g_origEndScene = nullptr;
    g_origReset = nullptr;
}

} // namespace skr::render
