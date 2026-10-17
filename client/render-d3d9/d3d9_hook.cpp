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
IntroRenderer g_fallbackIntro;
std::atomic<bool> g_initialized{false};
std::atomic<bool> g_hookInstalled{false};
HANDLE g_hookThread = nullptr;
LARGE_INTEGER g_lastTime{};
LARGE_INTEGER g_freq{};

// ── SKRIntro.dll dynamic loading ────────────────────────────────────
HMODULE g_skrIntroDll = nullptr;

using SKR_InitializeFn  = BOOL(__cdecl*)(HWND);
using SKR_FrameFn       = void(__cdecl*)(IDirect3DDevice9*);
using SKR_BeforeResetFn = void(__cdecl*)();
using SKR_AfterResetFn  = void(__cdecl*)(HRESULT);
using SKR_SkipFn        = void(__cdecl*)();
using SKR_IsFinishedFn  = BOOL(__cdecl*)();
using SKR_ShutdownFn    = void(__cdecl*)();

SKR_InitializeFn  g_skrInitialize  = nullptr;
SKR_FrameFn       g_skrFrame       = nullptr;
SKR_BeforeResetFn g_skrBeforeReset = nullptr;
SKR_AfterResetFn  g_skrAfterReset  = nullptr;
SKR_SkipFn        g_skrSkip        = nullptr;
SKR_IsFinishedFn  g_skrIsFinished  = nullptr;
SKR_ShutdownFn    g_skrShutdown    = nullptr;

bool g_useSkrIntro = false;
bool g_skrIntroInitialized = false;

bool LoadSkrIntro() {
    // Look for SKRIntro.dll next to the game executable
    char path[MAX_PATH]{};
    GetModuleFileNameA(nullptr, path, MAX_PATH);

    // Replace exe filename with SKRIntro.dll
    char* lastSlash = strrchr(path, '\\');
    if (!lastSlash) return false;
    strcpy(lastSlash + 1, "SKRIntro.dll");

    g_skrIntroDll = LoadLibraryExA(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!g_skrIntroDll) return false;

    g_skrInitialize  = reinterpret_cast<SKR_InitializeFn>(GetProcAddress(g_skrIntroDll, "SKR_Initialize"));
    g_skrFrame       = reinterpret_cast<SKR_FrameFn>(GetProcAddress(g_skrIntroDll, "SKR_Frame"));
    g_skrBeforeReset = reinterpret_cast<SKR_BeforeResetFn>(GetProcAddress(g_skrIntroDll, "SKR_BeforeReset"));
    g_skrAfterReset  = reinterpret_cast<SKR_AfterResetFn>(GetProcAddress(g_skrIntroDll, "SKR_AfterReset"));
    g_skrSkip        = reinterpret_cast<SKR_SkipFn>(GetProcAddress(g_skrIntroDll, "SKR_Skip"));
    g_skrIsFinished  = reinterpret_cast<SKR_IsFinishedFn>(GetProcAddress(g_skrIntroDll, "SKR_IsFinished"));
    g_skrShutdown    = reinterpret_cast<SKR_ShutdownFn>(GetProcAddress(g_skrIntroDll, "SKR_Shutdown"));

    if (!g_skrInitialize || !g_skrFrame || !g_skrBeforeReset ||
        !g_skrAfterReset || !g_skrIsFinished || !g_skrShutdown) {
        FreeLibrary(g_skrIntroDll);
        g_skrIntroDll = nullptr;
        return false;
    }

    return true;
}

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
    if (g_useSkrIntro) {
        // Initialize SKRIntro on first frame
        if (!g_skrIntroInitialized) {
            HWND hwnd = nullptr;
            D3DDEVICE_CREATION_PARAMETERS params{};
            if (SUCCEEDED(device->GetCreationParameters(&params)))
                hwnd = params.hFocusWindow;
            if (hwnd && g_skrInitialize(hwnd))
                g_skrIntroInitialized = true;
        }

        if (g_skrIntroInitialized && !g_skrIsFinished())
            g_skrFrame(device);
    } else {
        // Fallback: simple intro renderer
        if (!g_initialized.load()) {
            if (g_fallbackIntro.CreateResources(device)) {
                QueryPerformanceFrequency(&g_freq);
                QueryPerformanceCounter(&g_lastTime);
                g_initialized.store(true);
            }
        }

        if (g_initialized.load() && !g_fallbackIntro.IsDone()) {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            float dt = static_cast<float>(now.QuadPart - g_lastTime.QuadPart)
                       / static_cast<float>(g_freq.QuadPart);
            g_lastTime = now;
            if (dt > 0.1f) dt = 0.1f;

            for (int vk = 0x01; vk <= 0xFE; ++vk) {
                if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON) continue;
                if (GetAsyncKeyState(vk) & 0x0001) {
                    g_fallbackIntro.Skip();
                    break;
                }
            }

            g_fallbackIntro.Draw(device, dt);
        }
    }

    return g_origEndScene(device);
}

HRESULT WINAPI HookedReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* pp) {
    if (g_useSkrIntro && g_skrIntroInitialized)
        g_skrBeforeReset();
    else
        g_fallbackIntro.BeforeReset();

    HRESULT hr = g_origReset(device, pp);

    if (g_useSkrIntro && g_skrIntroInitialized) {
        g_skrAfterReset(hr);
    } else if (SUCCEEDED(hr)) {
        g_fallbackIntro.AfterReset(device);
        QueryPerformanceCounter(&g_lastTime);
    }

    return hr;
}

DWORD WINAPI D3D9HookThread(LPVOID) {
    // Try to load SKRIntro.dll (optional — fallback to simple intro)
    g_useSkrIntro = LoadSkrIntro();

    auto** ppDevice = reinterpret_cast<IDirect3DDevice9**>(kGtaDevicePtr);

    // Poll until GTA creates its D3D9 device
    for (int i = 0; i < 600; ++i) {
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

    if (g_useSkrIntro && g_skrIntroInitialized && g_skrShutdown)
        g_skrShutdown();
    else
        g_fallbackIntro.Shutdown();

    g_origEndScene = nullptr;
    g_origReset = nullptr;
    g_hookInstalled.store(false);
}

} // namespace skr::render
