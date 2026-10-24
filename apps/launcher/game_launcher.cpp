#include "game_launcher.h"
#include "ipc/launch_params.h"
#include "bootstrap/init_state.h"
#include <cstdio>
#include <thread>

namespace skr::launcher {

const char* LaunchResultToString(LaunchResult r) {
    switch (r) {
        case LaunchResult::Success:             return "Success";
        case LaunchResult::GtaNotFound:         return "gta_sa.exe not found";
        case LaunchResult::DllNotFound:         return "SKR.Client.dll not found";
        case LaunchResult::ProcessCreateFailed: return "CreateProcessW failed";
        case LaunchResult::InjectionFailed:     return "DLL injection failed";
        case LaunchResult::PipeFailed:          return "Named pipe IPC failed";
        case LaunchResult::StartupFilesFailed: return "Startup DLL conflict or restore failed; inspect samp.dll/d3d9.dll and .skr backups";
        case LaunchResult::ResumeThreadFailed:  return "ResumeThread failed";
        case LaunchResult::NativeInitializationFailed: return "Native hook initialization failed";
        case LaunchResult::NativeInitializationTimeout: return "Native hook initialization timed out";
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

    const DWORD waited = WaitForSingleObject(hThread, 10000);
    if (waited != WAIT_OBJECT_0) {
        // The remote thread may still use remoteMem. The caller terminates
        // this newly created process on failure, which reclaims the allocation.
        CloseHandle(hThread);
        return nullptr;
    }

    DWORD exitCode = 0;
    const BOOL completed = GetExitCodeThread(hThread, &exitCode);
    CloseHandle(hThread);
    VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);

    return completed && exitCode != STILL_ACTIVE
        ? reinterpret_cast<HMODULE>(static_cast<uintptr_t>(exitCode)) : nullptr;
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

namespace {
bool CallRemoteDword(HANDLE process, LPTHREAD_START_ROUTINE proc, DWORD& result) {
    HANDLE thread = CreateRemoteThread(process, nullptr, 0, proc, nullptr, 0, nullptr);
    if (!thread) return false;
    const DWORD wait = WaitForSingleObject(thread, 2000);
    const BOOL read = wait == WAIT_OBJECT_0 && GetExitCodeThread(thread, &result);
    CloseHandle(thread);
    return read == TRUE;
}
}

LaunchResult LaunchGame(const LaunchConfig& config, DWORD& pidOut,
                        HANDLE& hProcessOut) {
    pidOut = 0;
    hProcessOut = nullptr;

    // Progress is reported only after the corresponding step actually
    // succeeded, so a milestone can never run ahead of the real state.
    auto step = [&config](LaunchStep s) { if (config.onStep) config.onStep(s); };

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

    // Keep proxy files out of process import resolution only. Never replace
    // an existing backup. Restore before explicit SKR initialization.
    struct StartupFiles {
        std::vector<std::pair<std::wstring, std::wstring>> moved;
        bool Restore() {
            bool ok = true;
            for (auto it = moved.rbegin(); it != moved.rend(); ++it) {
                if (GetFileAttributesW(it->second.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
                if (!MoveFileExW(it->second.c_str(), it->first.c_str(), 0)) ok = false;
            }
            return ok;
        }
        ~StartupFiles() { Restore(); }
        bool Hide(const std::wstring& original) {
            const auto backup = original + L".skr";
            if (GetFileAttributesW(backup.c_str()) != INVALID_FILE_ATTRIBUTES) return false;
            if (GetFileAttributesW(original.c_str()) == INVALID_FILE_ATTRIBUTES) return true;
            if (!MoveFileExW(original.c_str(), backup.c_str(), 0)) return false;
            moved.emplace_back(original, backup);
            return true;
        }
    } startupFiles;
    if (!startupFiles.Hide(workDir + L"\\samp.dll") ||
        !startupFiles.Hide(workDir + L"\\d3d9.dll")) return LaunchResult::StartupFilesFailed;

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
    step(LaunchStep::ProcessCreated);

    // Inject SKR.Client.dll — lobby, AC, CEF overlay
    HMODULE remoteModule = InjectDll(pi.hProcess, config.clientDllPath.c_str());
    if (!remoteModule) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return LaunchResult::InjectionFailed;
    }

    if (!startupFiles.Restore()) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return LaunchResult::StartupFilesFailed;
    }
    step(LaunchStep::ClientInjected);

    // Resolve request and status exports. SkrRemoteInit only accepts the
    // request; it does not claim that runtime-dependent hooks are ready.
    auto remoteInit = FindRemoteExport(remoteModule, config.clientDllPath.c_str(), "SkrRemoteInit");
    auto remoteState = FindRemoteExport(remoteModule, config.clientDllPath.c_str(), "SkrGetInitializationState");
    auto remoteError = FindRemoteExport(remoteModule, config.clientDllPath.c_str(), "SkrGetInitializationError");
    if (!remoteInit || !remoteState || !remoteError) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return LaunchResult::InjectionFailed;
    }

    // Build IPC params — server address/port are no longer set at launch;
    // samp_loader connects dynamically when the user picks a server (Phase 2).
    ipc::LaunchParams params{};
    params.magic   = ipc::kLaunchParamsMagic;
    params.version = ipc::kLaunchParamsVersion;
    params.serverPort = config.serverPort;
    params.enhancedAdvertised = config.enhancedAdvertised ? 1 : 0;
    params.sessionId = config.sessionId;
    auto duplicateEvent = [&](HANDLE source, uint32_t& target, DWORD access) {
        if (!source) return true;
        HANDLE remote = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), source, pi.hProcess, &remote, access, FALSE, 0)) return false;
        target = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(remote));
        return true;
    };
    if (!duplicateEvent(config.frameReady, params.frameReadyHandle, EVENT_MODIFY_STATE) ||
        !duplicateEvent(config.takeover, params.takeoverHandle, SYNCHRONIZE) ||
        !duplicateEvent(config.sessionReady, params.sessionReadyHandle, EVENT_MODIFY_STATE) ||
        !duplicateEvent(config.gameplayConfirmed, params.gameplayConfirmedHandle, EVENT_MODIFY_STATE) ||
        !duplicateEvent(config.clientInitialized, params.clientInitializedHandle, EVENT_MODIFY_STATE) ||
        !duplicateEvent(config.sampLoaded, params.sampLoadedHandle, EVENT_MODIFY_STATE) ||
        !duplicateEvent(config.sampReady, params.sampReadyHandle, EVENT_MODIFY_STATE)) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return LaunchResult::PipeFailed;
    }
    strncpy_s(params.serverAddress, config.serverAddress.c_str(), _TRUNCATE);
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

    // Accept initialization while GTA is suspended. D3D9 and SA-MP cannot
    // finish until the game runs, so readiness is polled only after resume.
    HANDLE hInitThread = CreateRemoteThread(pi.hProcess, nullptr, 0,
                                            remoteInit, nullptr, 0, nullptr);
    if (!hInitThread) {
        pipeThread.join();
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return LaunchResult::InjectionFailed;
    }

    const DWORD initWait = WaitForSingleObject(hInitThread, 10000);
    pipeThread.join();
    SecureZeroMemory(&params, sizeof(params));

    DWORD initResult = 1;
    const BOOL initCompleted = GetExitCodeThread(hInitThread, &initResult);
    CloseHandle(hInitThread);

    if (!pipeOk) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return LaunchResult::PipeFailed;
    }

    if (initWait != WAIT_OBJECT_0 || !initCompleted || initResult != 0) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return LaunchResult::InjectionFailed;
    }
    // Resume GTA so it can create D3D9 and SA-MP runtime objects.
    if (ResumeThread(pi.hThread) == static_cast<DWORD>(-1)) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return LaunchResult::ResumeThreadFailed;
    }

    step(LaunchStep::GameResumed);

    const DWORD pollStarted = GetTickCount();
    bool ready = false;
    DWORD lastReportedState = MAXDWORD;
    while (GetTickCount() - pollStarted < config.nativeInitializationTimeoutMs) {
        DWORD stateValue = 0;
        if (!CallRemoteDword(pi.hProcess, remoteState, stateValue)) {
            TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return LaunchResult::NativeInitializationFailed;
        }
        const auto state = static_cast<skr::bootstrap::InitializationState>(stateValue);
        if (stateValue > static_cast<DWORD>(skr::bootstrap::InitializationState::FAILED)) {
            TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return LaunchResult::NativeInitializationFailed;
        }
        if (stateValue != lastReportedState) {
            lastReportedState = stateValue;
            if (config.onNativeState) config.onNativeState(stateValue, 0);
        }
        if (state == skr::bootstrap::InitializationState::HOOKS_INSTALLED) {
            ready = true;
            break;
        }
        if (state == skr::bootstrap::InitializationState::FAILED) {
            DWORD errorValue = 0;
            CallRemoteDword(pi.hProcess, remoteError, errorValue);
            if (config.onNativeState) config.onNativeState(stateValue, errorValue);
            char message[128]{};
            sprintf_s(message, "SKR native initialization failed (error %lu)\n", errorValue);
            OutputDebugStringA(message);
            TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return LaunchResult::NativeInitializationFailed;
        }
        Sleep(50);
    }
    if (!ready) {
        if (config.onNativeState) {
            config.onNativeState(static_cast<uint32_t>(
                skr::bootstrap::InitializationState::FAILED),
                static_cast<uint32_t>(skr::bootstrap::InitializationError::INITIALIZATION_TIMEOUT));
        }
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return LaunchResult::NativeInitializationTimeout;
    }
    step(LaunchStep::ClientReady);

    CloseHandle(pi.hThread);
    hProcessOut = pi.hProcess;
    return LaunchResult::Success;
}

} // namespace skr::launcher
