// Local development probe. Uses the same LaunchGame path as the launcher.
#include "game_launcher.h"
#include <cstdio>
int wmain(int argc, wchar_t** argv) {
    if (argc != 3) {
        puts("Usage: SKRLaunchProbe <gta_sa.exe> <SKR.Client.dll> (localhost:7777 only)");
        return 2;
    }
    skr::launcher::LaunchConfig config;
    config.gtaSaPath = argv[1]; config.clientDllPath = argv[2];
    config.serverAddress = "127.0.0.1"; config.serverPort = 7777;
    config.playerName = L"SKR_Test";
    DWORD pid = 0; HANDLE process = nullptr;
    const auto result = skr::launcher::LaunchGame(config, pid, process);
    printf("Launch result: %s; child pid: %lu\n", skr::launcher::LaunchResultToString(result), pid);
    fflush(stdout);
    if (result != skr::launcher::LaunchResult::Success) return 1;
    const DWORD waited = WaitForSingleObject(process, 45000);
    if (waited == WAIT_OBJECT_0) {
        DWORD code = 0; GetExitCodeProcess(process, &code);
        printf("Game exited: 0x%08lX\n", code);
    } else {
        puts("Game still running after 45 seconds; left open for interactive verification.");
    }
    CloseHandle(process);
    return 0;
}
