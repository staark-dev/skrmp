#include "game_launcher.h"
#include "ipc/launch_params.h"
#include <cstdio>
#include <thread>

namespace skr::launcher {

const char* LaunchResultToString(LaunchResult r) {
    switch (r) {
        case LaunchResult::Success:             return "Success";
        case LaunchResult::GtaNotFound:         return "gta_sa.exe not found";
        case LaunchResult::DllNotFound:         return "SKR.Client.dll not found";
        case LaunchResult::SampNotFound:       return "samp.dll not found";
        case LaunchResult::ProcessCreateFailed: return "CreateProcessW failed";
        case LaunchResult::InjectionFailed:     return "DLL injection failed";
        case LaunchResult::PipeFailed:          return "Named pipe IPC failed";
        case LaunchResult::ResumeThreadFailed:  return "ResumeThread failed";
    }
    return "Unknown";
}

HMODULE InjectDll(HANDLE hProcess, const wchar_t* dllPath) {
    size_t pathBytes = (wcslen(dllPath) + 1) * sizeof(wchar_t);

    void* remoteMem = VirtualAllocEx(hProcess, nullptr, pathBytes,
                                     MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteMem) return nullptr;

    if (!WriteProcessMemory(hProcess, remoteMem, dllPath, pathBytes, nullptr)) {
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        return nullptr;
    }

    auto loadLibAddr = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
    if (!loadLibAddr) {
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        return nullptr;
    }

    HANDLE hThread = CreateRemoteThread(hProcess, nullptr, 0,
                                        loadLibAddr, remoteMem, 0, nullptr);
    if (!hThread) {
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        return nullptr;
    }

    WaitForSingleObject(hThread, 10000);

    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);
    CloseHandle(hThread);
    VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);

    return reinterpret_cast<HMODULE>(static_cast<uintptr_t>(exitCode));
}

LPTHREAD_START_ROUTINE FindRemoteExport(HMODULE remoteBase,
                                         const wchar_t* dllPath,
                                         const char* exportName) {
    HMODULE localDll = LoadLibraryExW(dllPath, nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!localDll) return nullptr;

    auto localProc = reinterpret_cast<uintptr_t>(GetProcAddress(localDll, exportName));
    auto localBase = reinterpret_cast<uintptr_t>(localDll);
    FreeLibrary(localDll);

    if (!localProc) return nullptr;
    uintptr_t rva = localProc - localBase;
    return reinterpret_cast<LPTHREAD_START_ROUTINE>(
        reinterpret_cast<uintptr_t>(remoteBase) + rva);
}

LaunchResult LaunchGame(const LaunchConfig& config, DWORD& pidOut) {
    pidOut = 0;

    // Validate paths
    if (GetFileAttributesW(config.gtaSaPath.c_str()) == INVALID_FILE_ATTRIBUTES)
        return LaunchResult::GtaNotFound;
    if (GetFileAttributesW(config.clientDllPath.c_str()) == INVALID_FILE_ATTRIBUTES)
        return LaunchResult::DllNotFound;
    if (GetFileAttributesW(config.sampDllPath.c_str()) == INVALID_FILE_ATTRIBUTES)
        return LaunchResult::SampNotFound;

    // Extract working directory from gta_sa.exe path
    std::wstring workDir = config.gtaSaPath;
    auto pos = workDir.rfind(L'\\');
    if (pos != std::wstring::npos)
        workDir.resize(pos);

    // SA-MP's samp.dll reads these command line args for auto-connect:
    //   -c         = connect mode
    //   -h <IP>    = server address
    //   -p <PORT>  = server port
    //   -n <NAME>  = player name
    //   -z         = close on disconnect
    std::wstring cmdLine = L"\"" + config.gtaSaPath + L"\""
        + L" -c -h " + config.serverAddress
        + L" -p " + std::to_wstring(config.serverPort)
        + L" -n " + config.playerName;

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

    // Inject samp.dll first — it provides RakNet networking to the open.mp server
    HMODULE remoteSamp = InjectDll(pi.hProcess, config.sampDllPath.c_str());
    if (!remoteSamp) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return LaunchResult::SampNotFound;
    }

    // Inject SKR.Client.dll — our intro, AC, CEF overlay
    HMODULE remoteModule = InjectDll(pi.hProcess, config.clientDllPath.c_str());
    if (!remoteModule) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return LaunchResult::InjectionFailed;
    }

    // Find SkrRemoteInit in the remote process (same DLL, different base address)
    auto remoteInit = FindRemoteExport(remoteModule, config.clientDllPath.c_str(), "SkrRemoteInit");
    if (!remoteInit) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return LaunchResult::InjectionFailed;
    }

    // Build IPC params
    ipc::LaunchParams params{};
    params.magic   = ipc::kLaunchParamsMagic;
    params.version = ipc::kLaunchParamsVersion;
    params.serverPort = config.serverPort;
    WideCharToMultiByte(CP_UTF8, 0, config.serverAddress.c_str(), -1,
                        params.serverAddress, sizeof(params.serverAddress), nullptr, nullptr);
    WideCharToMultiByte(CP_UTF8, 0, config.playerName.c_str(), -1,
                        params.playerName, sizeof(params.playerName), nullptr, nullptr);
    if (config.joinTicket.size() == ipc::kJoinTicketSize) {
        memcpy(params.joinTicket, config.joinTicket.data(), ipc::kJoinTicketSize);
        params.hasJoinTicket = 1;
    }

    // Pipe writer runs concurrently with SkrRemoteInit in the game process.
    // WriteLaunchParams blocks until the DLL's ReadLaunchParams connects.
    bool pipeOk = false;
    std::thread pipeThread([&]() {
        pipeOk = ipc::WriteLaunchParams(pi.dwProcessId, params);
    });

    // Call SkrRemoteInit in the game process — it reads the pipe, hooks D3D9, starts AC
    HANDLE hInitThread = CreateRemoteThread(pi.hProcess, nullptr, 0,
                                            remoteInit, nullptr, 0, nullptr);
    if (!hInitThread) {
        pipeThread.join();
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return LaunchResult::InjectionFailed;
    }

    WaitForSingleObject(hInitThread, 10000);
    pipeThread.join();

    DWORD initResult = 1;
    GetExitCodeThread(hInitThread, &initResult);
    CloseHandle(hInitThread);

    if (!pipeOk) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return LaunchResult::PipeFailed;
    }

    // Resume game's main thread — D3D9 hooks are installed, intro will render
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
