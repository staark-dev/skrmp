#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#include "render-d3d9/d3d9_hook.h"
#include "skrac/mock_adapter.h"
#include "skrac/ac_policy.h"
#include "ipc/launch_params.h"

namespace {
    std::atomic<bool> started{false};
    skr::ac::MockAcAdapter g_acAdapter;
    skr::ac::AcPolicyEnforcer* g_acPolicy = nullptr;
    skr::ipc::LaunchParams g_launchParams{};
    bool g_hasLaunchParams = false;
}

extern "C" __declspec(dllexport) BOOL __cdecl SkrInitialize() noexcept {
    if (started.exchange(true)) return TRUE;

    // Read launch params from launcher via named pipe.
    // In fake_game / dev mode this may fail — we continue with defaults.
    g_hasLaunchParams = skr::ipc::ReadLaunchParams(g_launchParams, 3000);

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

extern "C" __declspec(dllexport) BOOL __cdecl SkrGetLaunchParams(
    skr::ipc::LaunchParams* out) noexcept {
    if (!g_hasLaunchParams || !out) return FALSE;
    *out = g_launchParams;
    return TRUE;
}

extern "C" __declspec(dllexport) DWORD WINAPI SkrRemoteInit(LPVOID) noexcept {
    return SkrInitialize() ? 0 : 1;
}
#pragma comment(linker, "/EXPORT:SkrRemoteInit=_SkrRemoteInit@4")

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
