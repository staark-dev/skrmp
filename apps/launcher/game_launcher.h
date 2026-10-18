#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace skr::launcher {

struct LaunchConfig {
    std::wstring gtaSaPath;      // Full path to gta_sa.exe
    std::wstring clientDllPath;  // Full path to SKR.Client.dll
    std::wstring playerName;
    std::vector<uint8_t> joinTicket; // 32 bytes join ticket from backend (empty = no ticket yet)
};

enum class LaunchResult : uint8_t {
    Success,
    GtaNotFound,
    DllNotFound,
    ProcessCreateFailed,
    InjectionFailed,
    PipeFailed,
    ResumeThreadFailed
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
