#include "samp-loader/samp_loader.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>

namespace skr::samp_loader {

namespace {

std::atomic<bool> g_loaded{false};
std::mutex g_loadMutex;
HMODULE g_sampModule = nullptr;

std::wstring LogDir() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&LogDir), &self);
    wchar_t p[MAX_PATH];
    DWORD n = GetModuleFileNameW(self, p, MAX_PATH);
    if (!n || n >= MAX_PATH) return {};
    std::wstring s(p, n);
    auto slash = s.find_last_of(L'\\');
    if (slash == std::wstring::npos) return {};
    return s.substr(0, slash + 1) + L"SKR\\";
}

void Log(const char* fmt, ...) {
    std::wstring dir = LogDir();
    if (dir.empty()) return;
    CreateDirectoryW(dir.c_str(), nullptr);
    FILE* f = _wfopen((dir + L"samp_loader.log").c_str(), L"a");
    if (!f) return;
    fprintf(f, "[%lu] ", GetTickCount());
    va_list ap; va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

std::wstring GtaDir() {
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring path(exe);
    auto slash = path.rfind(L'\\');
    if (slash != std::wstring::npos) path.resize(slash);
    return path;
}

} // namespace

bool LoadSamp() {
    std::lock_guard<std::mutex> lk(g_loadMutex);
    if (g_loaded.load()) return true;

    // The launcher hides the SA-MP loading chain (renamed to .skr).
    // Restore all three before loading samp.dll.
    std::wstring dir = GtaDir();
    struct RestoreEntry { const wchar_t* hidden; const wchar_t* orig; };
    RestoreEntry restoreList[] = {
        { L"\\vorbisHooked.dll.skr", L"\\vorbisHooked.dll" },
        { L"\\d3d9.dll.skr",         L"\\d3d9.dll" },
        { L"\\samp.dll.skr",         L"\\samp.dll" },
    };
    for (auto& e : restoreList) {
        std::wstring hidden = dir + e.hidden;
        std::wstring orig   = dir + e.orig;
        if (GetFileAttributesW(hidden.c_str()) != INVALID_FILE_ATTRIBUTES)
            MoveFileExW(hidden.c_str(), orig.c_str(), MOVEFILE_REPLACE_EXISTING);
    }

    std::wstring sampPath = dir + L"\\samp.dll";
    if (GetFileAttributesW(sampPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Log("samp.dll not found (tried restore from .skr and original)");
        return false;
    }

    Log("loading samp from %ls", sampPath.c_str());
    g_sampModule = LoadLibraryW(sampPath.c_str());
    if (!g_sampModule) {
        Log("LoadLibrary FAILED: error %lu", GetLastError());
        return false;
    }

    g_loaded.store(true);
    Log("samp.dll loaded at 0x%p", g_sampModule);
    return true;
}

bool IsSampLoaded() {
    return g_loaded.load();
}

bool Connect(const ConnectParams& params) {
    if (!g_loaded.load() || !g_sampModule) {
        Log("Connect called but samp.dll not loaded");
        return false;
    }
    if (!params.serverAddress || !params.playerName) {
        Log("Connect: null address or name");
        return false;
    }

    // SA-MP 0.3.7-R5 offsets (GTA SA 1.0 US only):
    //   samp.dll + 0x26EB94 → CNetGame* pointer
    //   CNetGame + 0x025 → char serverAddress[257]
    //   CNetGame + 0x126 → uint16_t serverPort
    //   CNetGame + 0x3CD → uint8_t state (5 = connected)
    //
    // To trigger a connect:
    //   1. Write server address and port to netGame
    //   2. Call CNetGame::Connect() or transition state to CONNECTING
    //
    // Phase 2 TODO: find CNetGame::Connect() RVA and call it directly.
    // For now, log the intent and return false (not yet implemented).

    Log("Connect requested: %s:%u name=%s",
        params.serverAddress, params.serverPort, params.playerName);
    Log("Phase 2: CNetGame::Connect() call not yet implemented");
    return false;
}

} // namespace skr::samp_loader
