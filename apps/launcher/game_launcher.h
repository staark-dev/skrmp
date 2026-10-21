#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>
#include <functional>

namespace skr::launcher {

// Coarse launch milestones, reported while LaunchGame runs. Each one is a
// fact observed in this process -- never an estimate and never a percentage.
enum class LaunchStep : uint8_t {
    ProcessCreated,  // CreateProcessW returned; the game is suspended
    ClientInjected,  // SKR.Client.dll is loaded in the target process
    ClientReady,     // SkrRemoteInit returned 0 (hooks installed, AC up)
    GameResumed      // main thread resumed; the cover can start rendering
};

// Invoked on the caller's (worker) thread as each milestone is reached. It
// must not block or touch UI: the launcher marshals these onto its UI timer.
using LaunchProgressCallback = std::function<void(LaunchStep)>;

struct LaunchConfig {
    std::wstring gtaSaPath;      // Full path to gta_sa.exe
    std::wstring clientDllPath;  // Full path to SKR.Client.dll
    std::string serverAddress;
    uint16_t serverPort = 0;
    std::wstring playerName;
    std::vector<uint8_t> joinTicket; // 32 bytes join ticket from backend (empty = no ticket yet)
    HANDLE frameReady = nullptr;
    HANDLE takeover = nullptr;
    HANDLE sessionReady = nullptr;
    bool enhancedAdvertised = false; // Fresh native query, not a UI claim.
    bool localHandoffTest = false; // Explicit launcher-only development switch.
    LaunchProgressCallback onStep; // Optional progress sink; empty = no reporting.
};

enum class LaunchResult : uint8_t {
    Success,
    GtaNotFound,
    DllNotFound,
    ProcessCreateFailed,
    InjectionFailed,
    PipeFailed,
    ResumeThreadFailed,
    StartupFilesFailed
};

const char* LaunchResultToString(LaunchResult r);

// Launches GTA SA in suspended state, injects SKR.Client.dll, then resumes.
// The DLL is responsible for connecting to the open.mp server.
// On success, hProcessOut receives an open handle the caller must CloseHandle.
LaunchResult LaunchGame(const LaunchConfig& config, DWORD& pidOut,
                        HANDLE& hProcessOut);

// Injects a DLL into a target process using CreateRemoteThread + LoadLibraryW.
// Returns the remote HMODULE, or nullptr on failure.
HMODULE InjectDll(HANDLE hProcess, const wchar_t* dllPath);

// Resolves an exported function in the remote process by RVA calculation.
LPTHREAD_START_ROUTINE FindRemoteExport(HMODULE remoteBase,
                                         const wchar_t* dllPath,
                                         const char* exportName);

} // namespace skr::launcher
