#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#include "render-d3d9/d3d9_hook.h"
#include "skrac/mock_adapter.h"
#include "skrac/ac_policy.h"

namespace {
    std::atomic<bool> started{false};
    skr::ac::MockAcAdapter g_acAdapter;
    skr::ac::AcPolicyEnforcer* g_acPolicy = nullptr;
}

extern "C" __declspec(dllexport) BOOL __cdecl SkrInitialize() noexcept {
    if (started.exchange(true)) return TRUE;

    if (!skr::render::InstallD3D9Hooks()) {
        started.store(false);
        return FALSE;
    }

    static skr::ac::PolicyConfig config{};
    config.mode = skr::ac::AcEnforcement::Development;
    static skr::ac::AcPolicyEnforcer policy(g_acAdapter, config);
    g_acPolicy = &policy;

    if (!g_acPolicy->Initialize("dev-session")) {
        skr::render::RemoveD3D9Hooks();
        started.store(false);
        return FALSE;
    }

    return TRUE;
}

extern "C" __declspec(dllexport) void __cdecl SkrShutdown() noexcept {
    if (g_acPolicy) {
        g_acPolicy->Shutdown();
        g_acPolicy = nullptr;
    }
    skr::render::RemoveD3D9Hooks();
    started.store(false);
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) {
    return TRUE; // No CEF, D3D, network, threads or waits here.
}
