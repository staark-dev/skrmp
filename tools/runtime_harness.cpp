#include "game_launcher.h"
#include "apps/launcher/preflight/exe_fingerprint.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {
constexpr const char* kCanonicalHoodlum =
    "A559AA772FD136379155EFA71F00C47AAD34BBFEAE6196B0FE1047D0645CBD26";
constexpr const char* kPinnedSampR5 =
    "B72B5DBE725F81864CA3F78BC7063BDA56CC05FC7188AF822FA7A754432553A2";

FILE* g_log = nullptr;

void Log(const char* format, ...) {
    SYSTEMTIME now{};
    GetSystemTime(&now);
    char prefix[64]{};
    sprintf_s(prefix, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ ", now.wYear, now.wMonth,
              now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
    std::fputs(prefix, stdout);
    if (g_log) std::fputs(prefix, g_log);
    va_list args;
    va_start(args, format);
    std::vprintf(format, args);
    va_end(args);
    va_start(args, format);
    if (g_log) std::vfprintf(g_log, format, args);
    va_end(args);
    std::fputc('\n', stdout);
    if (g_log) { std::fputc('\n', g_log); std::fflush(g_log); }
}

const char* StepName(skr::launcher::LaunchStep step) {
    using skr::launcher::LaunchStep;
    switch (step) {
        case LaunchStep::ProcessCreated: return "ProcessCreated";
        case LaunchStep::ClientInjected: return "ClientInjected";
        case LaunchStep::GameResumed: return "GameResumed";
        case LaunchStep::ClientReady: return "ClientReady";
    }
    return "Unknown";
}

const char* StateName(uint32_t state) {
    static const char* names[] = {
        "NOT_STARTED", "INITIALIZING", "WAITING_FOR_D3D9",
        "WAITING_FOR_SAMP", "HOOKS_INSTALLED", "FAILED"};
    return state < _countof(names) ? names[state] : "INVALID";
}

std::wstring DirectoryOf(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : path.substr(0, slash);
}

HMODULE FindRemoteModule(DWORD pid, const wchar_t* name) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE) return nullptr;
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    HMODULE result = nullptr;
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szModule, name) == 0) {
                result = entry.hModule;
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

BOOL CALLBACK CloseGameWindow(HWND window, LPARAM processId) {
    DWORD owner = 0;
    GetWindowThreadProcessId(window, &owner);
    if (owner == static_cast<DWORD>(processId) && IsWindowVisible(window))
        PostMessageW(window, WM_CLOSE, 0, 0);
    return TRUE;
}

bool RemoteShutdown(HANDLE process, DWORD pid, const std::wstring& clientPath) {
    HMODULE module = FindRemoteModule(pid, L"SKR.Client.dll");
    if (!module) return false;
    auto shutdown = skr::launcher::FindRemoteExport(module, clientPath.c_str(), "SkrRemoteShutdown");
    if (!shutdown) return false;
    HANDLE thread = CreateRemoteThread(process, nullptr, 0, shutdown, nullptr, 0, nullptr);
    if (!thread) return false;
    const DWORD wait = WaitForSingleObject(thread, 10000);
    DWORD result = 1;
    const bool ok = wait == WAIT_OBJECT_0 && GetExitCodeThread(thread, &result) && result == 0;
    CloseHandle(thread);
    return ok;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 4 || argc > 5) {
        std::puts("Usage: SKRRuntimeHarness <isolated-gta.exe> <SKR.Client.dll> <log-dir> [timeout-ms]");
        return 2;
    }
    const std::wstring gtaPath = argv[1];
    const std::wstring clientPath = argv[2];
    const std::wstring logDir = argv[3];
    const DWORD timeoutMs = argc == 5 ? wcstoul(argv[4], nullptr, 10) : 40000;

    CreateDirectoryW(logDir.c_str(), nullptr);
    const std::wstring logPath = logDir + L"\\harness.log";
    g_log = _wfopen(logPath.c_str(), L"a");
    if (!g_log) return 3;

    Log("HARNESS_START timeout_ms=%lu", timeoutMs);
    const std::wstring gtaDir = DirectoryOf(gtaPath);
    const std::wstring clientDir = DirectoryOf(clientPath);
    if (GetFileAttributesW((gtaDir + L"\\.skr-c1.6.2-isolated").c_str()) == INVALID_FILE_ATTRIBUTES) {
        Log("REFUSED isolation marker missing");
        fclose(g_log);
        return 4;
    }

    const std::string gtaHash = skr::preflight::ComputeFileSha256(gtaPath);
    const std::wstring sampPath = clientDir + L"\\samp\\samp.dll";
    const std::string sampHash = skr::preflight::ComputeFileSha256(sampPath);
    Log("INPUT gta_sha256=%s", gtaHash.c_str());
    Log("INPUT samp_sha256=%s", sampHash.c_str());
    if (_stricmp(gtaHash.c_str(), kCanonicalHoodlum) != 0) {
        Log("REFUSED executable is not canonical Hoodlum");
        fclose(g_log);
        return 5;
    }
    if (_stricmp(sampHash.c_str(), kPinnedSampR5) != 0) {
        Log("REFUSED samp.dll is not pinned R5");
        fclose(g_log);
        return 6;
    }

    skr::launcher::LaunchConfig config{};
    config.gtaSaPath = gtaPath;
    config.clientDllPath = clientPath;
    config.serverAddress = "127.0.0.1";
    config.serverPort = 7777;
    config.playerName = L"SKR_Runtime_Test";
    config.nativeInitializationTimeoutMs = timeoutMs;
    config.onStep = [](skr::launcher::LaunchStep step) { Log("STEP %s", StepName(step)); };
    config.onNativeState = [](uint32_t state, uint32_t error) {
        Log("NATIVE_STATE %s(%lu) error=%lu", StateName(state), state, error);
    };

    DWORD pid = 0;
    HANDLE process = nullptr;
    const auto result = skr::launcher::LaunchGame(config, pid, process);
    Log("LAUNCH_RESULT %s pid=%lu", skr::launcher::LaunchResultToString(result), pid);
    if (result != skr::launcher::LaunchResult::Success) {
        fclose(g_log);
        return result == skr::launcher::LaunchResult::NativeInitializationTimeout ? 20 : 10;
    }

    Sleep(3000);
    const bool shutdown = RemoteShutdown(process, pid, clientPath);
    Log("REMOTE_SHUTDOWN %s", shutdown ? "PASS" : "FAIL");
    EnumWindows(&CloseGameWindow, static_cast<LPARAM>(pid));
    DWORD wait = WaitForSingleObject(process, 10000);
    if (wait != WAIT_OBJECT_0) {
        Log("WINDOW_CLOSE_TIMEOUT forcing isolated child termination");
        TerminateProcess(process, 0xC162);
        WaitForSingleObject(process, 5000);
    }
    DWORD exitCode = STILL_ACTIVE;
    GetExitCodeProcess(process, &exitCode);
    Log("PROCESS_EXIT code=0x%08lX graceful=%s", exitCode,
        wait == WAIT_OBJECT_0 ? "yes" : "no");
    CloseHandle(process);
    fclose(g_log);
    return shutdown && wait == WAIT_OBJECT_0 ? 0 : 30;
}
