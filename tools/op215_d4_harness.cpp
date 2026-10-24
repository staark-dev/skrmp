// OP215 D4/D5 Test Harness — minimal GTA injection for controlled runtime test.
// Launches gta_sa.exe suspended, injects SKR.Client.dll (v6 launch params),
// and resumes. No monitoring, no vehicle pool patches — the DLL handles everything.
//
// Usage: op215_d4_harness.exe <gta_sa.exe> <SKR.Client.dll> [--port <port>]

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include "ipc/launch_params.h"

namespace {

HMODULE InjectDll(HANDLE process, const wchar_t* path) {
    const size_t bytes = (wcslen(path) + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!remote || !WriteProcessMemory(process, remote, path, bytes, nullptr)) return nullptr;
    auto loader = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
    HANDLE thread = CreateRemoteThread(process, nullptr, 0, loader, remote, 0, nullptr);
    if (!thread) return nullptr;
    DWORD wait = WaitForSingleObject(thread, 15000);
    DWORD result = 0;
    BOOL read = GetExitCodeThread(thread, &result);
    CloseHandle(thread);
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    return wait == WAIT_OBJECT_0 && read && result != STILL_ACTIVE
        ? reinterpret_cast<HMODULE>(static_cast<uintptr_t>(result)) : nullptr;
}

LPTHREAD_START_ROUTINE FindRemoteExport(HMODULE remoteBase, const wchar_t* path, const char* name) {
    HMODULE local = LoadLibraryExW(path, nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!local) return nullptr;
    auto proc = reinterpret_cast<uintptr_t>(GetProcAddress(local, name));
    auto base = reinterpret_cast<uintptr_t>(local);
    FreeLibrary(local);
    return proc ? reinterpret_cast<LPTHREAD_START_ROUTINE>(
        reinterpret_cast<uintptr_t>(remoteBase) + proc - base) : nullptr;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: op215_d4_harness <gta_sa.exe> <SKR.Client.dll> [--port <port>]\n");
        return 2;
    }
    uint16_t serverPort = 7778;
    for (int i = 3; i < argc; ++i) {
        if (wcscmp(argv[i], L"--port") == 0 && i + 1 < argc)
            serverPort = static_cast<uint16_t>(wcstoul(argv[++i], nullptr, 10));
    }
    std::wstring gta = argv[1];
    std::wstring client = argv[2];
    std::wstring dir = gta.substr(0, gta.find_last_of(L"\\/"));
    std::wstring samp = dir + L"\\samp.dll";
    std::wstring hidden = samp + L".skr";

    std::printf("OP215 D4/D5 Test Harness\n");
    std::printf("  GTA:    %ls\n", gta.c_str());
    std::printf("  Client: %ls\n", client.c_str());
    std::printf("  Server: 127.0.0.1:%u\n", serverPort);

    // Hide samp.dll during CreateProcess to prevent import-resolution loading.
    if (GetFileAttributesW(samp.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (!MoveFileExW(samp.c_str(), hidden.c_str(), 0)) {
            std::fprintf(stderr, "FAIL: cannot hide samp.dll\n");
            return 3;
        }
    }

    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + gta + L"\"";
    BOOL created = CreateProcessW(gta.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                                  CREATE_SUSPENDED, nullptr, dir.c_str(), &si, &pi);
    // Restore samp.dll immediately.
    if (GetFileAttributesW(hidden.c_str()) != INVALID_FILE_ATTRIBUTES)
        MoveFileExW(hidden.c_str(), samp.c_str(), 0);

    if (!created) { std::fprintf(stderr, "FAIL: CreateProcess\n"); return 4; }
    std::printf("PROCESS pid=%lu suspended\n", pi.dwProcessId);

    // Inject SKR.Client.dll.
    HMODULE remoteModule = InjectDll(pi.hProcess, client.c_str());
    if (!remoteModule) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        std::fprintf(stderr, "FAIL: DLL injection\n");
        return 5;
    }
    std::printf("INJECTED module=0x%08IX\n", reinterpret_cast<uintptr_t>(remoteModule));

    auto remoteInit = FindRemoteExport(remoteModule, client.c_str(), "SkrRemoteInit");
    if (!remoteInit) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        std::fprintf(stderr, "FAIL: SkrRemoteInit not found\n");
        return 6;
    }

    // Send v6 launch params via named pipe.
    skr::ipc::LaunchParams params{};
    params.magic   = skr::ipc::kLaunchParamsMagic;
    params.version = skr::ipc::kLaunchParamsVersion;
    params.serverPort = serverPort;
    params.enhancedAdvertised = 1;
    std::strcpy(params.serverAddress, "127.0.0.1");
    std::strcpy(params.playerName, "OP215_D4");

    bool pipeOk = false;
    std::thread pipeThread([&] {
        pipeOk = skr::ipc::WriteLaunchParams(pi.dwProcessId, params, 10000);
    });

    HANDLE initThread = CreateRemoteThread(pi.hProcess, nullptr, 0, remoteInit, nullptr, 0, nullptr);
    if (!initThread) {
        pipeThread.join();
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        std::fprintf(stderr, "FAIL: CreateRemoteThread\n");
        return 7;
    }

    DWORD initWait = WaitForSingleObject(initThread, 15000);
    pipeThread.join();
    DWORD initExit = STILL_ACTIVE;
    GetExitCodeThread(initThread, &initExit);
    CloseHandle(initThread);

    std::printf("INIT wait=0x%lX pipe=%d exit=%lu\n", initWait, pipeOk ? 1 : 0, initExit);
    if (initWait != WAIT_OBJECT_0 || !pipeOk || initExit != 0) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        std::fprintf(stderr, "FAIL: SkrRemoteInit (pipe=%d exit=%lu)\n", pipeOk, initExit);
        return 8;
    }

    // Resume GTA main thread.
    ResumeThread(pi.hThread);
    std::printf("RESUMED — GTA is running.\n");
    std::printf("  Press F9 in-game to spawn vehicles (cycles 612 -> 613).\n");
    std::printf("  Press F8 to toggle diagnostic logging.\n");
    std::printf("  Logs: <gta>\\SKR\\op215.log\n");
    std::printf("  Close this window or press Ctrl+C to terminate.\n\n");

    // Wait for GTA to exit.
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    std::printf("GTA exited: 0x%08lX\n", exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
