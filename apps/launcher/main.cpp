#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "updater.h"
#include "inventory.h"

static std::wstring GetLauncherDir() {
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring path(buf);
    auto pos = path.rfind(L'\\');
    if (pos != std::wstring::npos)
        path.resize(pos);
    return path;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    auto baseDir = GetLauncherDir();
    auto stagingDir = baseDir + L"\\.skr-staging";
    auto inventoryPath = baseDir + L"\\.skr-inventory.dat";

    // Load file inventory
    skr::updater::FileInventory inventory(inventoryPath.c_str());
    inventory.Load();

    // Build current state from inventory
    skr::updater::ReleaseManifest currentManifest;
    inventory.BuildCurrentManifest(currentManifest);

    // Initialize update engine
    skr::updater::UpdateEngine engine(baseDir.c_str(), stagingDir.c_str());

    // TODO: fetch manifest from GET /v1/releases/stable
    // TODO: verify manifest signature
    // TODO: download changed files to staging
    // TODO: engine.SetManifest(remoteManifest)
    // TODO: engine.PlanUpdate(currentManifest)
    // TODO: engine.VerifyStaging()
    // TODO: engine.ApplyUpdate()
    // TODO: update inventory with new entries
    // TODO: authenticate user and obtain join ticket
    // TODO: launch game with DLL injection

    MessageBoxW(nullptr,
                L"SKR Launcher: updater engine ready.\n"
                L"Manifest, inventory and staging systems initialized.\n"
                L"Authentication and game launch not yet implemented.",
                L"SKR Client", MB_OK | MB_ICONINFORMATION);
    return 0;
}
