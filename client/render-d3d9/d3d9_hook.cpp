#include "render-d3d9/d3d9_hook.h"
#include "render-d3d9/intro_renderer.h"
#include <atomic>

namespace skr::render {

namespace {

// GTA SA stores the RenderWare D3D9 device pointer at this address (v1.0 / Steam classic)
constexpr uintptr_t kGtaDevicePtr = 0x00C97C28;

EndSceneFn g_origEndScene = nullptr;
ResetFn    g_origReset    = nullptr;
void**     g_hookedVtable = nullptr;
IntroRenderer g_intro;
std::atomic<bool> g_initialized{false};
std::atomic<bool> g_hookInstalled{false};
HANDLE g_hookThread = nullptr;
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
        if (dt > 0.1f) dt = 0.1f;

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

DWORD WINAPI D3D9HookThread(LPVOID) {
    auto** ppDevice = reinterpret_cast<IDirect3DDevice9**>(kGtaDevicePtr);

    // Poll until GTA creates its D3D9 device (game main thread must be resumed)
    for (int i = 0; i < 600; ++i) { // 30 seconds max
        if (*ppDevice != nullptr) break;
        Sleep(50);
    }

    IDirect3DDevice9* device = *ppDevice;
    if (!device) return 1;

    g_hookedVtable = *reinterpret_cast<void***>(device);

    bool ok = true;
    ok &= PatchVtable(g_hookedVtable, kVtableEndScene,
                       reinterpret_cast<void*>(&HookedEndScene),
                       reinterpret_cast<void**>(&g_origEndScene));
    ok &= PatchVtable(g_hookedVtable, kVtableReset,
                       reinterpret_cast<void*>(&HookedReset),
                       reinterpret_cast<void**>(&g_origReset));

    g_hookInstalled.store(ok);
    return ok ? 0 : 1;
}

} // anonymous namespace

bool InstallD3D9Hooks() {
    g_hookThread = CreateThread(nullptr, 0, D3D9HookThread, nullptr, 0, nullptr);
    return g_hookThread != nullptr;
}

void RemoveD3D9Hooks() {
    if (g_hookThread) {
        WaitForSingleObject(g_hookThread, 5000);
        CloseHandle(g_hookThread);
        g_hookThread = nullptr;
    }
    if (g_hookInstalled.load() && g_hookedVtable) {
        if (g_origEndScene) {
            DWORD old = 0;
            void* entry = &g_hookedVtable[kVtableEndScene];
            VirtualProtect(entry, sizeof(void*), PAGE_READWRITE, &old);
            g_hookedVtable[kVtableEndScene] = reinterpret_cast<void*>(g_origEndScene);
            VirtualProtect(entry, sizeof(void*), old, &old);
        }
        if (g_origReset) {
            DWORD old = 0;
            void* entry = &g_hookedVtable[kVtableReset];
            VirtualProtect(entry, sizeof(void*), PAGE_READWRITE, &old);
            g_hookedVtable[kVtableReset] = reinterpret_cast<void*>(g_origReset);
            VirtualProtect(entry, sizeof(void*), old, &old);
        }
    }
    g_intro.Shutdown();
    g_origEndScene = nullptr;
    g_origReset = nullptr;
    g_hookInstalled.store(false);
}

} // namespace skr::render
