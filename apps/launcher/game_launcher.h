#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <string>

namespace skr::launcher {

struct LaunchConfig {
    std::wstring gtaSaPath;      // Full path to gta_sa.exe
    std::wstring clientDllPath;  // Full path to SKR.Client.dll
    std::wstring serverAddress;  // e.g. "127.0.0.1"
    uint16_t serverPort = 7777;
    std::wstring playerName;
};

enum class LaunchResult : uint8_t {
    Success,
    GtaNotFound,
    DllNotFound,
    ProcessCreateFailed,
    InjectionFailed,
    ResumeThreadFailed
};

const char* LaunchResultToString(LaunchResult r);

// Launches GTA SA in suspended state, injects SKR.Client.dll, then resumes.
// The DLL is responsible for connecting to the open.mp server.
LaunchResult LaunchGame(const LaunchConfig& config, DWORD& pidOut);

// Injects a DLL into a target process using CreateRemoteThread + LoadLibraryW.
// Process must have PROCESS_ALL_ACCESS or equivalent.
bool InjectDll(HANDLE hProcess, const wchar_t* dllPath);

} // namespace skr::launcher
