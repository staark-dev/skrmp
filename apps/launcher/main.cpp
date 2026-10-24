#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "updater.h"
#include "inventory.h"
#include "game_launcher.h"
#ifdef SKR_EXTERNAL_LOBBY
#include "lobby_window.h"
#include "preflight_window.h"
#include "lobby/settings_io.h"
#include "preflight/preflight_controller.h"
#endif
#include "../../shared/exit_codes.h"
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

#ifndef SKR_EXTERNAL_LOBBY
static std::wstring Utf8ToWide(const std::string& value) {
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) return L"Connection rejected by the server.";
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), length);
    return result;
}
#endif

#ifdef SKR_EXTERNAL_LOBBY
// Narrow/wide helpers for the stored path. Unlike Utf8ToWide above, these
// return empty on failure: a filesystem path must never fall back to
// placeholder text that could then be probed or launched.
static std::wstring PathToWide(const std::string& value) {
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(static_cast<size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), result.data(), length) != length) return {};
    return result;
}

static std::string PathToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string result(static_cast<size_t>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
            result.data(), length, nullptr, nullptr) != length) return {};
    return result;
}

// A usable installation is a *regular* gta_sa.exe: present and not a
// directory. Applied to the configured and picked paths; the relative and
// well-known lookups below keep their original checks unchanged.
static bool IsGtaExe(const std::wstring& path) {
    if (path.empty()) return false;
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

// Accepts either a full gta_sa.exe path or the folder containing it.
static std::wstring ResolveGtaCandidate(const std::wstring& configured) {
    if (configured.empty()) return {};
    if (IsGtaExe(configured)) return configured;
    const std::wstring withExe = configured + L"\\gta_sa.exe";
    return IsGtaExe(withExe) ? withExe : std::wstring();
}

static bool HasCompleteNativeApproval(const skr::preflight::PreflightReport& report) {
    const skr::preflight::CheckResult* checks[] = {
        &report.gtaDiscovery, &report.exeVerification, &report.sampInspection,
        &report.depVerification, &report.bundledRuntime
    };
    for (const auto* check : checks) {
        if (check->status == skr::preflight::CheckStatus::Pending ||
            skr::preflight::IsBlocking(check->status)) return false;
    }
    return true;
}
#endif

static constexpr const wchar_t* kGtaSaSearchPaths[] = {
    L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Grand Theft Auto San Andreas",
};

static std::wstring FindGtaSa(const std::wstring& baseDir) {
#ifdef SKR_EXTERNAL_LOBBY
    // The player's saved installation wins. It is the only location that
    // survives the launcher being installed to an arbitrary folder, so it is
    // consulted before the relative and well-known paths below. gtaPath is
    // written only by this process: SettingsFromJson deliberately ignores it,
    // so neither the lobby page nor server content can steer what we launch.
    skr::lobby::Settings settings;
    if (skr::lobby::LoadSettings(settings) && !settings.gtaPath.empty()) {
        const std::wstring configured = ResolveGtaCandidate(PathToWide(settings.gtaPath));
        if (!configured.empty()) return configured;
        // Stale or invalid: fall through to discovery rather than failing.
    }
#endif
    // Look in launcher directory first
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

    // Try well-known install locations
    for (auto* searchPath : kGtaSaSearchPaths) {
        candidate = std::wstring(searchPath) + L"\\gta_sa.exe";
        if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES)
            return candidate;
    }

    return {};
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR arguments, int) {
#ifdef SKR_EXTERNAL_LOBBY
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
#endif
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
#ifdef SKR_EXTERNAL_LOBBY
    if (arguments && wcscmp(arguments, L"--lobby-smoke") == 0) {
        auto smoke = skr::launcher::ShowLobbyWindow(instance, baseDir + L"\\SKR\\lobby", true);
        return smoke.smokePassed ? 0 : 2;
    }

#endif
    auto gtaPath = FindGtaSa(baseDir);

#ifdef SKR_EXTERNAL_LOBBY
    auto preflightUi = skr::launcher::ShowPreflightExperience(
        instance, baseDir + L"\\SKR\\preflight", baseDir, gtaPath);
    skr::preflight::PreflightReport preflightReport;
    if (preflightUi.webViewAvailable) {
        if (!preflightUi.approved) return 1;
        preflightReport = preflightUi.report;
    } else {
        // If WebView2 cannot initialize, mandatory verification still runs and
        // the existing native results window remains the safe recovery path.
        preflightReport = skr::preflight::RunPreflight(baseDir);
        skr::preflight::WriteDiagnosticLog(
            preflightReport, preflightReport.gtaDir, baseDir);
        if (!HasCompleteNativeApproval(preflightReport)) {
            if (!skr::preflight::ShowPreflightWindow(instance, preflightReport)) return 1;
        }
    }

    gtaPath = preflightReport.gtaExePath;
    if (!gtaPath.empty()) {
        skr::lobby::Settings settings;
        skr::lobby::LoadSettings(settings);
        settings.gtaPath = PathToUtf8(gtaPath);
        skr::lobby::SaveSettings(settings);
    }
#endif

    if (gtaPath.empty()) {
        MessageBoxW(nullptr,
                    L"gta_sa.exe not found.\n"
                    L"Place the SKR launcher in your GTA San Andreas folder\n"
                    L"or install GTA SA via Steam.",
                    L"SKR Client", MB_OK | MB_ICONERROR);
        return 1;
    }

    // Resolve the GTA directory from the found exe path
    std::wstring gtaDir = gtaPath;
    auto slashPos = gtaDir.rfind(L'\\');
    if (slashPos != std::wstring::npos)
        gtaDir.resize(slashPos);

    // Look for DLLs in launcher dir first, then GTA dir
    auto findDll = [&](const wchar_t* name) -> std::wstring {
        std::wstring path = baseDir + L"\\" + name;
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
            return path;
        path = gtaDir + L"\\" + name;
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
            return path;
        return {};
    };

    auto clientDll = findDll(L"SKR.Client.dll");
#ifdef SKR_EXTERNAL_LOBBY
    if (!preflightReport.skrClientPath.empty()) clientDll = preflightReport.skrClientPath;
#endif

    if (clientDll.empty()) {
        MessageBoxW(nullptr, L"SKR.Client.dll not found.\nPlace it next to the launcher or in the GTA SA folder.",
                    L"SKR Client", MB_OK | MB_ICONERROR);
        return 1;
    }

    skr::launcher::LaunchConfig config;
    config.gtaSaPath = gtaPath;
    config.clientDllPath = clientDll;
    config.playerName = L"SKR_Player";
#ifdef SKR_EXTERNAL_LOBBY
    config.localHandoffTest = arguments && wcscmp(arguments, L"--local-handoff-test") == 0;
    skr::launcher::ShowLobbyWindow(instance, baseDir + L"\\SKR\\lobby", false, &config);
    return 0;
#else
    for (;;) {
        DWORD gamePid = 0;
        HANDLE game = nullptr;
        auto result = skr::launcher::LaunchGame(config, gamePid, game);
        if (result != skr::launcher::LaunchResult::Success) {
            wchar_t message[512];
            swprintf_s(message, L"Failed to launch game: %S", skr::launcher::LaunchResultToString(result));
            MessageBoxW(nullptr, message, L"SKR Client", MB_OK | MB_ICONERROR);
            return 1;
        }
        WaitForSingleObject(game, INFINITE);
        DWORD exitCode = 0;
        GetExitCodeProcess(game, &exitCode);
        CloseHandle(game);
        const auto rejection = skr::RejectionForExitCode(exitCode);
        if (rejection != skr::protocol::RejectionReason::None) {
            const std::wstring message = Utf8ToWide(skr::protocol::RejectionDisplayText(
                rejection, skr::SupportedClientsForExitCode(exitCode)));
            MessageBoxW(nullptr, message.c_str(), L"SKR Connection Rejected", MB_OK | MB_ICONWARNING);
            continue;
        }
        if (exitCode != skr::kExitRestartLobby) return 0;
    }
#endif
}
