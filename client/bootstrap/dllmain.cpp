#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include "render-d3d9/d3d9_hook.h"
#include "samp-hooks/samp_hooks.h"
#include "lobby/lobby_manager.h"
#include "skrac/mock_adapter.h"
#include "skrac/ac_policy.h"
#include "ipc/launch_params.h"
#include "samp-loader/samp_loader.h"
#include "gta-fixes/gta_fixes.h"

namespace {
    std::atomic<bool> started{false};
    skr::ac::MockAcAdapter g_acAdapter;
    skr::ac::AcPolicyEnforcer* g_acPolicy = nullptr;
    skr::ipc::LaunchParams g_launchParams{};
    bool g_hasLaunchParams = false;
    LONG CALLBACK CrashTraceHandler(EXCEPTION_POINTERS* info);
}

extern "C" __declspec(dllexport) BOOL __cdecl SkrInitialize() noexcept {
    if (started.exchange(true)) return TRUE;

    // Read launch params from launcher via named pipe.
    // In fake_game / dev mode this may fail — we continue with defaults.
    g_hasLaunchParams = skr::ipc::ReadLaunchParams(g_launchParams, 3000);

    // Runs before the launcher resumes GTA's main thread, like stock SA-MP.
    skr::samp_loader::PreloadSamp();

    AddVectoredExceptionHandler(1, &CrashTraceHandler);
    skr::gta_fixes::InstallGtaFixes();

    if (!skr::render::InstallD3D9Hooks()) {
        started.store(false);
        return FALSE;
    }

    skr::samp_hooks::InstallSampHooks();
    skr::lobby::Initialize();

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
    skr::lobby::Shutdown();
    skr::samp_hooks::RemoveSampHooks();
    skr::render::RemoveD3D9Hooks();
    started.store(false);
}

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
