#include "ipc/launch_params.h"
#include <thread>
#include <cstdio>
#include <chrono>

int main() {
    using namespace skr::ipc;
    int checks = 0, failures = 0;
    auto check = [&](bool ok, const char* name) {
        ++checks; if (!ok) ++failures;
        printf("[%s] %s\n", ok ? "OK" : "FAIL", name);
    };
    LaunchParams params{};
    params.magic = kLaunchParamsMagic; params.version = kLaunchParamsVersion;
    strcpy_s(params.serverAddress, "127.0.0.1"); params.serverPort = 7777;
    strcpy_s(params.playerName, "Ipc_Test");
    const DWORD pid = GetCurrentProcessId();
    for (int i = 0; i < 10; ++i) {
        bool wrote = false;
        std::thread writer([&] { wrote = WriteLaunchParams(pid, params, 2000); });
        LaunchParams received{};
        bool read = ReadLaunchParams(received, 2000);
        writer.join();
        check(wrote && read && memcmp(&params, &received, sizeof(params)) == 0,
              "round trip and receipt acknowledgement");
    }
    auto start = GetTickCount64();
    check(!WriteLaunchParams(pid, params, 150), "absent reader fails");
    check(GetTickCount64() - start < 1500, "absent reader is bounded");
    LaunchParams received{};
    start = GetTickCount64();
    check(!ReadLaunchParams(received, 150), "absent writer fails");
    check(GetTickCount64() - start < 1500, "absent writer is bounded");

    // A peer connects but never reads or acknowledges. This previously hung
    // indefinitely in FlushFileBuffers, despite the advertised timeout.
    bool wrote = true;
    std::thread writer([&] { wrote = WriteLaunchParams(pid, params, 250); });
    wchar_t name[128]; MakePipeName(name, 128, pid);
    HANDLE stalled = INVALID_HANDLE_VALUE;
    const auto deadline = GetTickCount64() + 200;
    do {
        stalled = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (stalled != INVALID_HANDLE_VALUE) break;
        Sleep(1);
    } while (GetTickCount64() < deadline);
    start = GetTickCount64();
    writer.join();
    check(stalled != INVALID_HANDLE_VALUE && !wrote, "connected peer without acknowledgement rejected");
    check(GetTickCount64() - start < 1500, "stalled peer is bounded");
    if (stalled != INVALID_HANDLE_VALUE) CloseHandle(stalled);

    params.version = 1;
    check(!WriteLaunchParams(pid, params, 150), "old transport version rejected");
    params.version = kLaunchParamsVersion;
    memset(params.playerName, 'A', sizeof(params.playerName));
    check(!WriteLaunchParams(pid, params, 150), "unterminated nickname rejected");
    printf("Launch IPC: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
