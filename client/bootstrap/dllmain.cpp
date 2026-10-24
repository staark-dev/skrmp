#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include "render-d3d9/d3d9_hook.h"
#include "samp-hooks/samp_hooks.h"
#include "lobby/lobby_manager.h"
#include "skrx/skrx_client.h"
#include "skrac/mock_adapter.h"
#include "skrac/ac_policy.h"
#include "ipc/launch_params.h"
#include "samp-loader/samp_loader.h"
#include "gta-fixes/gta_fixes.h"
#include "intro/launch_cover.h"
#include "intro/launch_signals.h"
#include "gps/gps_engine.h"
#include "game-state/game_state.h"
#include "native-pause/native_pause.h"
#include "bootstrap/init_state.h"
#ifdef SKR_OP215
#include "vehicle/custom_vehicle.h"
#endif

namespace {
    std::atomic<bool> started{false};
    skr::ac::MockAcAdapter g_acAdapter;
    skr::ac::AcPolicyEnforcer* g_acPolicy = nullptr;
    skr::ipc::LaunchParams g_launchParams{};
    bool g_hasLaunchParams = false;
    skr::bootstrap::InitializationTracker g_initialization;
    std::atomic<bool> g_monitorShutdown{false};
    HANDLE g_monitorThread = nullptr;
    LONG CALLBACK CrashTraceHandler(EXCEPTION_POINTERS* info);

    // samp.dll identity for the SAMP_LOADED trace: PE timestamp + image size of
    // the pinned 0.3.7-R5 build (same values as native-pause/pause_input.cpp).
    const char* SampBuild() {
        const auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(L"samp.dll"));
        if (!base) return "samp.dll not loaded";
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return "samp.dll unknown";
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        return nt->Signature == IMAGE_NT_SIGNATURE && nt->FileHeader.TimeDateStamp == 0x6372C39E &&
               nt->OptionalHeader.SizeOfImage == 0x27E000 ? "samp.dll 0.3.7-R5" : "samp.dll unknown build";
    }

    DWORD WINAPI InitializationMonitor(LPVOID) {
        constexpr DWORD kTimeoutMs = 35000;
        const DWORD startedAt = GetTickCount();
        while (!g_monitorShutdown.load(std::memory_order_acquire)) {
            const auto d3d9 = skr::render::GetD3D9HookState();
            const auto samp = skr::samp_hooks::GetSampHookState();
            if (d3d9 == skr::bootstrap::ComponentState::FAILED) {
                g_initialization.Fail(skr::bootstrap::InitializationError::D3D9_HOOK_FAILED);
                return 1;
            }
            if (samp == skr::bootstrap::ComponentState::FAILED) {
                g_initialization.Fail(skr::bootstrap::InitializationError::SAMP_HOOK_FAILED);
                return 1;
            }
            g_initialization.SetD3D9(d3d9);
            g_initialization.SetSamp(samp);
            if (d3d9 == skr::bootstrap::ComponentState::INSTALLED &&
                samp == skr::bootstrap::ComponentState::INSTALLED) {
                skr::launch_signals::Signal(skr::ipc::Milestone::SampLoaded, SampBuild());
                return 0;
            }
            if (GetTickCount() - startedAt >= kTimeoutMs) {
                g_initialization.Fail(skr::bootstrap::InitializationError::INITIALIZATION_TIMEOUT);
                return 1;
            }
            Sleep(25);
        }
        return 1;
    }
}

extern "C" __declspec(dllexport) BOOL __cdecl SkrUsesExternalHandoff() noexcept {
    return g_launchParams.frameReadyHandle != 0;
}
extern "C" __declspec(dllexport) BOOL __cdecl SkrInitialize() noexcept {
    if (started.exchange(true)) return TRUE;
    g_initialization.Begin();
    g_monitorShutdown.store(false, std::memory_order_release);

    // Read launch params from launcher via named pipe.
    // In fake_game / dev mode this may fail — we continue with defaults.
    g_hasLaunchParams = skr::ipc::ReadLaunchParams(g_launchParams, 3000);

    // Runs before the launcher resumes GTA's main thread, like stock SA-MP.
#ifdef SKR_EXTERNAL_LOBBY
    if (!g_hasLaunchParams || !g_launchParams.serverPort ||
        !memchr(g_launchParams.serverAddress, 0, sizeof(g_launchParams.serverAddress)) ||
        !memchr(g_launchParams.playerName, 0, sizeof(g_launchParams.playerName))) {
        g_initialization.Fail(skr::bootstrap::InitializationError::LAUNCH_PARAMS_INVALID);
        started.store(false);
        return FALSE;
    }
    skr::samp_loader::ConnectParams selected{g_launchParams.serverAddress,
        g_launchParams.serverPort, g_launchParams.playerName};
    if (!skr::samp_loader::PreloadSamp(&selected)) {
        g_initialization.Fail(skr::bootstrap::InitializationError::SAMP_PRELOAD_FAILED);
        started.store(false);
        return FALSE;
    }
    skr::launch_cover::Configure(g_launchParams.frameReadyHandle, g_launchParams.takeoverHandle);
    skr::launch_signals::Configure(g_launchParams);
#else
    skr::samp_loader::PreloadSamp();
#endif

    skr::game_state::ConfigureEnhancedDiscovery(g_hasLaunchParams && g_launchParams.enhancedAdvertised == 1);

    AddVectoredExceptionHandler(1, &CrashTraceHandler);
    skr::gta_fixes::InstallGtaFixes();

    if (!skr::render::InstallD3D9Hooks()) {
        g_initialization.Fail(skr::bootstrap::InitializationError::D3D9_THREAD_START_FAILED);
        started.store(false);
        return FALSE;
    }

    skr::native_pause::Initialize();

    if (!skr::samp_hooks::InstallSampHooks()) {
        g_initialization.Fail(skr::bootstrap::InitializationError::SAMP_THREAD_START_FAILED);
        skr::render::RemoveD3D9Hooks();
        started.store(false);
        return FALSE;
    }
    skr::lobby::Initialize();
    skr::skrx::Init();

    static skr::ac::PolicyConfig config{};
    config.mode = skr::ac::AcEnforcement::Development;
    static skr::ac::AcPolicyEnforcer policy(g_acAdapter, config);
    g_acPolicy = &policy;

    if (!g_acPolicy->Initialize("dev-session")) {
        g_initialization.Fail(skr::bootstrap::InitializationError::AC_INITIALIZATION_FAILED);
        skr::samp_hooks::RemoveSampHooks();
        skr::render::RemoveD3D9Hooks();
        started.store(false);
        return FALSE;
    }

    g_monitorThread = CreateThread(nullptr, 0, &InitializationMonitor, nullptr, 0, nullptr);
    if (!g_monitorThread) {
        g_initialization.Fail(skr::bootstrap::InitializationError::MONITOR_THREAD_START_FAILED);
        g_acPolicy->Shutdown();
        g_acPolicy = nullptr;
        skr::samp_hooks::RemoveSampHooks();
        skr::render::RemoveD3D9Hooks();
        started.store(false);
        return FALSE;
    }

    // Request accepted. Runtime readiness is reported separately and cannot
    // become HOOKS_INSTALLED until both asynchronous installers complete.
    skr::launch_signals::Signal(skr::ipc::Milestone::ClientInitialized);
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

extern "C" __declspec(dllexport) DWORD WINAPI SkrGetInitializationState(LPVOID) noexcept {
    return static_cast<DWORD>(g_initialization.GetState());
}
#pragma comment(linker, "/EXPORT:SkrGetInitializationState=_SkrGetInitializationState@4")

extern "C" __declspec(dllexport) DWORD WINAPI SkrGetInitializationError(LPVOID) noexcept {
    return static_cast<DWORD>(g_initialization.GetError());
}
#pragma comment(linker, "/EXPORT:SkrGetInitializationError=_SkrGetInitializationError@4")

extern "C" __declspec(dllexport) void __cdecl SkrShutdown() noexcept {
    g_monitorShutdown.store(true, std::memory_order_release);
    if (g_monitorThread) {
        WaitForSingleObject(g_monitorThread, 5000);
        CloseHandle(g_monitorThread);
        g_monitorThread = nullptr;
    }
    if (g_acPolicy) {
        g_acPolicy->Shutdown();
        g_acPolicy = nullptr;
    }
#ifdef SKR_OP215
    skr::vehicle::Shutdown();
#endif
    skr::native_pause::Shutdown();
    skr::gps::Shutdown();
    skr::lobby::Shutdown();
    skr::samp_hooks::RemoveSampHooks();
    skr::render::RemoveD3D9Hooks();
    g_initialization.Reset();
    started.store(false);
}

extern "C" __declspec(dllexport) DWORD WINAPI SkrRemoteShutdown(LPVOID) noexcept {
    SkrShutdown();
    return 0;
}
#pragma comment(linker, "/EXPORT:SkrRemoteShutdown=_SkrRemoteShutdown@4")

namespace {

// ── Diagnostics for silent process deaths ─────────────────────────────
// Both write to <gta>\SKR\<file>: a header line, then candidate return
// addresses (stack values that point into a loaded module) as module+offset.

void FormatAddress(char* out, size_t outLen, uintptr_t addr) {
    HMODULE mod = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(addr), &mod) && mod &&
        GetModuleFileNameA(mod, name, MAX_PATH)) {
        const char* base = strrchr(name, '\\');
        sprintf_s(out, outLen, "%08IX  %s+0x%IX", addr, base ? base + 1 : name,
                  addr - reinterpret_cast<uintptr_t>(mod));
    } else {
        sprintf_s(out, outLen, "%08IX  (no module)", addr);
    }
}

void WriteTrace(const wchar_t* file, const char* header, const uintptr_t* sp) {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash || (slash - path) + 32 >= MAX_PATH) return;
    swprintf_s(slash + 1, MAX_PATH - (slash + 1 - path), L"SKR\\%s", file);
    HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;

    auto write = [f](const char* s) {
        DWORD w = 0;
        WriteFile(f, s, static_cast<DWORD>(strlen(s)), &w, nullptr);
    };
    write(header);

    NT_TIB* tib = reinterpret_cast<NT_TIB*>(NtCurrentTeb());
    const auto* top = reinterpret_cast<const uintptr_t*>(tib->StackBase);
    char line[MAX_PATH + 64];
    int found = 0;
    for (; sp && sp < top && found < 40; ++sp) {
        HMODULE mod = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(*sp), &mod) || !mod)
            continue;
        char addr[MAX_PATH + 32];
        FormatAddress(addr, sizeof(addr), *sp);
        sprintf_s(line, "  %s\r\n", addr);
        write(line);
        ++found;
    }
    write("\r\n");
    CloseHandle(f);
}

// ExitProcess runs DLL_PROCESS_DETACH on the thread that called it, so its
// stack names the caller. Absent file after a silent exit = TerminateProcess.
void LogExitTrace() {
    char header[96];
    sprintf_s(header, "[%lu] process exit on thread %lu:\r\n", GetTickCount(), GetCurrentThreadId());
    WriteTrace(L"exit_trace.log", header, reinterpret_cast<const uintptr_t*>(&header));
}

// First-chance view of fatal exception types, before any filter decides what
// to do with them. Capped so legitimate first-chance faults cannot flood it.
std::atomic<int> g_crashTraceCount{0};

LONG CALLBACK CrashTraceHandler(EXCEPTION_POINTERS* info) {
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION: case EXCEPTION_STACK_OVERFLOW:
        case EXCEPTION_ILLEGAL_INSTRUCTION: case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_INT_DIVIDE_BY_ZERO: case 0xC0000409 /* fast fail */:
        case 0xC0000374 /* heap corruption */:
            break;
        default:
            return EXCEPTION_CONTINUE_SEARCH;
    }
    if (g_crashTraceCount.fetch_add(1) >= 30) return EXCEPTION_CONTINUE_SEARCH;

    char where[MAX_PATH + 32];
    FormatAddress(where, sizeof(where), reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress));
    char header[MAX_PATH + 192];
    const ULONG_PTR* p = info->ExceptionRecord->ExceptionInformation;
    sprintf_s(header, "[%lu] exception 0x%08lX on thread %lu at %s (info %IX %IX) eax=%08lX ecx=%08lX esi=%08lX edi=%08lX\r\n",
              GetTickCount(), code, GetCurrentThreadId(), where,
              info->ExceptionRecord->NumberParameters > 0 ? p[0] : 0,
              info->ExceptionRecord->NumberParameters > 1 ? p[1] : 0,
              info->ContextRecord->Eax, info->ContextRecord->Ecx,
              info->ContextRecord->Esi, info->ContextRecord->Edi);
    WriteTrace(L"crash_trace.log", header, reinterpret_cast<const uintptr_t*>(info->ContextRecord->Esp));
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID reserved) {
    // No CEF, D3D, network, threads or waits here.
    if (reason == DLL_PROCESS_DETACH && reserved)
        LogExitTrace();
    return TRUE;
}
