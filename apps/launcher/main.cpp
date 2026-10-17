#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "updater.h"
#include "inventory.h"
#include "game_launcher.h"
#include <cstdio>
#include <string>

static std::wstring GetLauncherDir() {
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring path(buf);
    auto pos = path.rfind(L'\\');
    if (pos != std::wstring::npos)
        path.resize(pos);
    return path;
}

static std::wstring FindGtaSa(const std::wstring& baseDir) {
    // Look for gta_sa.exe in same directory or known subdirectory
    std::wstring candidate = baseDir + L"\\gta_sa.exe";
    if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES)
        return candidate;

    // Try parent directory
    std::wstring parent = baseDir;
    auto pos = parent.rfind(L'\\');
    if (pos != std::wstring::npos) {
        parent.resize(pos);
        candidate = parent + L"\\gta_sa.exe";
        if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES)
            return candidate;
    }
    return {};
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    auto baseDir = GetLauncherDir();
    auto stagingDir = baseDir + L"\\.skr-staging";
    auto inventoryPath = baseDir + L"\\.skr-inventory.dat";

    // ── Update phase ─────────────────────────────────────
    skr::updater::FileInventory inventory(inventoryPath.c_str());
    inventory.Load();

    skr::updater::ReleaseManifest currentManifest;
    inventory.BuildCurrentManifest(currentManifest);

    skr::updater::UpdateEngine engine(baseDir.c_str(), stagingDir.c_str());

    // TODO: fetch manifest from GET /v1/releases/stable
    // TODO: verify manifest signature
    // TODO: download changed files to staging
    // TODO: engine.SetManifest(remoteManifest)
    // TODO: engine.PlanUpdate(currentManifest)
    // TODO: engine.VerifyStaging()
    // TODO: engine.ApplyUpdate()
    // TODO: update inventory with new entries

    // ── Auth phase ───────────────────────────────────────
    // TODO: authenticate user via backend REST API
    // TODO: obtain join ticket (joinId) for the target server
    // TODO: pass joinId + server address to client DLL via named pipe IPC

    // ── Launch phase ─────────────────────────────────────
    auto gtaPath = FindGtaSa(baseDir);
    auto clientDll = baseDir + L"\\SKR.Client.dll";

    if (gtaPath.empty()) {
        MessageBoxW(nullptr,
                    L"gta_sa.exe not found.\n"
                    L"Place the SKR launcher in your GTA San Andreas folder.",
                    L"SKR Client", MB_OK | MB_ICONERROR);
        return 1;
    }

    skr::launcher::LaunchConfig config;
    config.gtaSaPath = gtaPath;
    config.clientDllPath = clientDll;
    config.sampDllPath = baseDir + L"\\samp.dll";
    config.serverAddress = L"127.0.0.1";
    config.serverPort = 7777;
    config.playerName = L"SKR_Player";

    // TODO: read server address and player name from config / UI
    // TODO: pass auth ticket to client DLL via named pipe before resume

    DWORD gamePid = 0;
    auto result = skr::launcher::LaunchGame(config, gamePid);

    if (result != skr::launcher::LaunchResult::Success) {
        wchar_t msg[512];
        _snwprintf_s(msg, _TRUNCATE,
                     L"Failed to launch game: %S",
                     skr::launcher::LaunchResultToString(result));
        MessageBoxW(nullptr, msg, L"SKR Client", MB_OK | MB_ICONERROR);
        return 1;
    }

    return 0;
}
