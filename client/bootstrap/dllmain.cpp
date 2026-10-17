#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>

namespace { std::atomic<bool> started{false}; }

extern "C" __declspec(dllexport) BOOL __cdecl SkrInitialize() noexcept {
    if (started.exchange(true)) return TRUE;
    // TODO: explicit init steps with rollback; no hidden initialization.
    return TRUE;
}

extern "C" __declspec(dllexport) void __cdecl SkrShutdown() noexcept {
    started.store(false);
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) {
    return TRUE; // No CEF, D3D, network, threads or waits here.
}
