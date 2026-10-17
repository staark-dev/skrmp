#include "game_launcher.h"
#include <cstdio>

namespace skr::launcher {

const char* LaunchResultToString(LaunchResult r) {
    switch (r) {
        case LaunchResult::Success:             return "Success";
        case LaunchResult::GtaNotFound:         return "gta_sa.exe not found";
        case LaunchResult::DllNotFound:         return "SKR.Client.dll not found";
        case LaunchResult::ProcessCreateFailed: return "CreateProcessW failed";
        case LaunchResult::InjectionFailed:     return "DLL injection failed";
        case LaunchResult::ResumeThreadFailed:  return "ResumeThread failed";
    }
    return "Unknown";
}

bool InjectDll(HANDLE hProcess, const wchar_t* dllPath) {
    size_t pathBytes = (wcslen(dllPath) + 1) * sizeof(wchar_t);

    void* remoteMem = VirtualAllocEx(hProcess, nullptr, pathBytes,
                                     MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteMem) return false;

    if (!WriteProcessMemory(hProcess, remoteMem, dllPath, pathBytes, nullptr)) {
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        return false;
    }

    // LoadLibraryW is at the same address in every process (kernel32 is always mapped at same base)
    auto loadLibAddr = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
    if (!loadLibAddr) {
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        return false;
    }

    HANDLE hThread = CreateRemoteThread(hProcess, nullptr, 0,
                                        loadLibAddr, remoteMem, 0, nullptr);
    if (!hThread) {
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        return false;
    }

    WaitForSingleObject(hThread, 10000);

    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);
    CloseHandle(hThread);
    VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);

    // exitCode is the HMODULE returned by LoadLibraryW; 0 means failure
    return exitCode != 0;
}

LaunchResult LaunchGame(const LaunchConfig& config, DWORD& pidOut) {
    pidOut = 0;

    // Validate paths
    if (GetFileAttributesW(config.gtaSaPath.c_str()) == INVALID_FILE_ATTRIBUTES)
        return LaunchResult::GtaNotFound;
    if (GetFileAttributesW(config.clientDllPath.c_str()) == INVALID_FILE_ATTRIBUTES)
        return LaunchResult::DllNotFound;

    // Extract working directory from gta_sa.exe path
    std::wstring workDir = config.gtaSaPath;
    auto pos = workDir.rfind(L'\\');
    if (pos != std::wstring::npos)
        workDir.resize(pos);

    // Build command line — gta_sa.exe with server connection params
    // The client DLL reads these from the process environment or named pipe,
    // but we pass the server address as a command line hint for open.mp compatibility.
    std::wstring cmdLine = L"\"" + config.gtaSaPath + L"\"";

    // Create process in SUSPENDED state
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    BOOL created = CreateProcessW(
        config.gtaSaPath.c_str(),
        &cmdLine[0],
        nullptr, nullptr,
        FALSE,
        CREATE_SUSPENDED,
        nullptr,
        workDir.c_str(),
        &si, &pi
    );

    if (!created)
        return LaunchResult::ProcessCreateFailed;

    pidOut = pi.dwProcessId;

    // Inject SKR.Client.dll while process is suspended
    if (!InjectDll(pi.hProcess, config.clientDllPath.c_str())) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return LaunchResult::InjectionFailed;
    }

    // Resume main thread
    if (ResumeThread(pi.hThread) == static_cast<DWORD>(-1)) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return LaunchResult::ResumeThreadFailed;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return LaunchResult::Success;
}

} // namespace skr::launcher
