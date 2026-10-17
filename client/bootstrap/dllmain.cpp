#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#include "render-d3d9/d3d9_hook.h"

namespace { std::atomic<bool> started{false}; }

extern "C" __declspec(dllexport) BOOL __cdecl SkrInitialize() noexcept {
    if (started.exchange(true)) return TRUE;
    if (!skr::render::InstallD3D9Hooks()) {
        started.store(false);
        return FALSE;
    }
    return TRUE;
}

extern "C" __declspec(dllexport) void __cdecl SkrShutdown() noexcept {
    skr::render::RemoveD3D9Hooks();
    started.store(false);
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) {
    return TRUE; // No CEF, D3D, network, threads or waits here.
}
