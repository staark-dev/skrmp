#include "samp-loader/samp_loader.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <atomic>
#include <array>
#include <cstdio>
#include <mutex>
#include <string>

#ifdef SKR_TEST_CERTIFICATION
#include "vfs/samp_vfs.h"
#endif

namespace skr::samp_loader {

namespace {

std::atomic<bool> g_loaded{false};
std::mutex g_loadMutex;
HMODULE g_sampModule = nullptr;
std::atomic<bool> g_connectInvoked{false};

constexpr uintptr_t kR5SettingsRva = 0x26DFE8;
constexpr uintptr_t kR5SampGameRva = 0x26EBAC;  // g_SampObj (CGame*), initialization_flow.md §4.3

// gGameState: plugin-sdk CGame.cpp:18. 7 = frontend idle, the value
// Samp_MainThread waits for (initialization_flow.md §4).
constexpr uintptr_t kGtaGameState = 0x00C8D4C0;
constexpr int kGtaStateFrontendIdle = 7;
// `cmp dword ptr [0x00C8D4C0], 7` — verified in the pinned binary at both RVAs.
constexpr uint8_t kStartGameCompare[] = { 0x83, 0x3D, 0xC0, 0xD4, 0xC8, 0x00, 0x07 };
constexpr uintptr_t kStartGameCompareRvas[] = { 0xC3A8F, 0xC3AA4 };
std::atomic<int> g_startGate{0};
static_assert(sizeof(std::atomic<int>) == sizeof(int));
constexpr uintptr_t kR5NetGameRva  = 0x26EB94;
constexpr uintptr_t kR5ConnectRva  = 0x8940;
constexpr DWORD kR5Timestamp = 0x6372C39E;
constexpr DWORD kR5ImageSize = 0x27E000;
// Provenance for this exact R5 build:
// - deps/omp-cef/deps/samp-api 0.3.7-R5-1 Settings.cpp/CNetGame.cpp
// - deps/omp-cef SampVersionManager PE signature table
// - local static analysis in D:/SA-MP Reverse/RE/client/research/docs/initialization_flow.md
constexpr std::array<uint8_t, 32> kR5Sha256 = {
    0xB7,0x2B,0x5D,0xBE,0x72,0x5F,0x81,0x86,0x4C,0xA3,0xF7,0x8B,0xC7,0x06,0x3B,0xDA,
    0x56,0xCC,0x05,0xFC,0x71,0x88,0xAF,0x82,0x2F,0xA7,0xA7,0x54,0x43,0x25,0x53,0xA2
};

#pragma pack(push, 1)
struct R5Settings {
    BOOL debugMode;
    BOOL onlineGame;
    BOOL windowedMode;
    char password[257];
    char host[257];
    char port[257];
    char nickname[257];
    char debugScript[257];
};
#pragma pack(pop)
static_assert(sizeof(R5Settings) == 1297);

bool HashMatchesPinnedR5(const wchar_t* path) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::array<uint8_t, 32> digest{};
    bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
              BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == 0;
    std::array<uint8_t, 16384> buffer{};
    DWORD read = 0;
    while (ok) {
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
            ok = false;
            break;
        }
        if (!read) break;
        ok = BCryptHashData(hash, buffer.data(), read, 0) == 0;
    }
    if (ok) ok = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) == 0;
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(file);
    return ok && digest == kR5Sha256;
}

bool IsPinnedR5Image(HMODULE module) {
    const auto* base = reinterpret_cast<const uint8_t*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE &&
           nt->FileHeader.TimeDateStamp == kR5Timestamp &&
           nt->OptionalHeader.SizeOfImage == kR5ImageSize;
}

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

// R5 DllMain runs ParseCommandLine (GetCommandLineA) and stays inert without
// -c (initialization_flow.md §2.1 step 3, §3). GTA was started clean, so while
// samp.dll loads, GetCommandLineA must return the SA-MP flags.
// kernelbase!GetCommandLineA is `mov eax, [BaseAnsiCommandLine.Buffer]; ret`
// (A1 imm32 C3); kernel32's export may be a `jmp [iat]` (FF 25 imm32) stub to
// it. Only that exact shape is accepted; the global pointer is swapped, no
// code is patched.
char** FindAnsiCommandLinePointer() {
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    auto* p = k32 ? reinterpret_cast<const uint8_t*>(GetProcAddress(k32, "GetCommandLineA")) : nullptr;
    for (int hops = 0; p && hops < 3; ++hops) {
        if (p[0] == 0xFF && p[1] == 0x25) {
            p = **reinterpret_cast<const uint8_t* const* const*>(p + 2);
            continue;
        }
        if (p[0] == 0xA1 && p[5] == 0xC3)
            return *reinterpret_cast<char** const*>(p + 1);
        break;
    }
    return nullptr;
}

bool IsSafeArg(const char* s, bool allowDotDash) {
    if (!s || !*s) return false;
    for (; *s; ++s) {
        const char c = *s;
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == '_' || (allowDotDash && (c == '.' || c == '-'));
        if (!ok) return false;
    }
    return true;
}

std::wstring GtaDir() {
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring path(exe);
    auto slash = path.rfind(L'\\');
    if (slash != std::wstring::npos) path.resize(slash);
    return path;
}

// Directory holding SKR.Client.dll, with a trailing backslash.
// The bundled SA-MP runtime sits in "samp\\" beside it.
std::wstring SelfDir() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&SelfDir), &self);
    wchar_t p[MAX_PATH];
    DWORD n = GetModuleFileNameW(self, p, MAX_PATH);
    if (!n || n >= MAX_PATH) return {};
    std::wstring s(p, n);
    auto slash = s.find_last_of(L'\\');
    if (slash == std::wstring::npos) return {};
    return s.substr(0, slash + 1);
}

// Try to find samp.dll from a candidate path (plain or .skr-hidden).
// Returns the validated path or an empty string.
std::wstring FindSampAt(const std::wstring& candidatePath) {
    std::wstring hidden = candidatePath + L".skr";
    if (GetFileAttributesW(candidatePath.c_str()) == INVALID_FILE_ATTRIBUTES &&
        GetFileAttributesW(hidden.c_str()) != INVALID_FILE_ATTRIBUTES)
        MoveFileExW(hidden.c_str(), candidatePath.c_str(), 0);
    if (GetFileAttributesW(candidatePath.c_str()) == INVALID_FILE_ATTRIBUTES)
        return {};
    if (!HashMatchesPinnedR5(candidatePath.c_str())) {
        Log("samp.dll at %ls rejected: SHA-256 mismatch", candidatePath.c_str());
        return {};
    }
    return candidatePath;
}

#ifdef SKR_TEST_CERTIFICATION
// ── Error 1114 (ERROR_DLL_INIT_FAILED) Diagnostics ──────────────────
// samp.dll's DllMain returned FALSE. This diagnostic block gathers
// evidence about WHY the initialization failed, separately from the
// VFS redirection. Possible causes include:
//   1. Missing resource files (SAMP.img, fonts, etc.) — addressed by VFS
//   2. Missing or incompatible DLL dependencies
//   3. GTA process state not at the expected point for SA-MP bootstrap
//   4. SA-MP internal validation failure (version check, integrity, etc.)
//   5. bass.dll initialization failure (audio subsystem)
void DiagnoseError1114(const wchar_t* sampPath) {
    Log("=== ERROR 1114 DIAGNOSTICS ===");

    // 1. Check transitive DLL dependencies are loadable.
    const wchar_t* deps[] = {
        L"d3dx9_25.dll", L"COMCTL32.dll", L"ADVAPI32.dll",
        L"WSOCK32.dll", L"WINMM.dll", L"PSAPI.DLL",
        L"SHELL32.dll", L"USER32.dll", L"GDI32.dll",
        nullptr
    };
    for (const wchar_t** dep = deps; *dep; ++dep) {
        HMODULE h = GetModuleHandleW(*dep);
        if (!h) h = LoadLibraryExW(*dep, nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        Log("  dep %ls: %s (module=%p)", *dep, h ? "OK" : "MISSING", h);
    }

    // 2. Check bass.dll specifically (samp.dll imports it).
    //    bass.dll must be loadable from the samp.dll directory.
    std::wstring sampDir(sampPath);
    auto slash = sampDir.find_last_of(L'\\');
    if (slash != std::wstring::npos) {
        sampDir.resize(slash + 1);
        std::wstring bassPath = sampDir + L"bass.dll";
        DWORD attrs = GetFileAttributesW(bassPath.c_str());
        Log("  bass.dll at %ls: %s", bassPath.c_str(),
            attrs != INVALID_FILE_ATTRIBUTES ? "EXISTS" : "NOT FOUND");

        if (attrs != INVALID_FILE_ATTRIBUTES) {
            // Try loading bass.dll explicitly to see if IT fails.
            HMODULE bass = LoadLibraryExW(bassPath.c_str(), nullptr,
                                          LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                          LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            if (bass) {
                Log("  bass.dll: loaded successfully at %p", bass);
                FreeLibrary(bass);
            } else {
                Log("  bass.dll: LOAD FAILED (error %lu)", GetLastError());
            }
        }
    }

    // 3. Check GTA process state.
    const int gameState = *reinterpret_cast<volatile int*>(kGtaGameState);
    Log("  gGameState=%d (expected < %d)", gameState, kGtaStateFrontendIdle);

    // 4. Check if SA-MP resource files exist at expected locations.
    std::wstring gtaDir = GtaDir();
    const wchar_t* resources[] = {
        L"\\SAMP\\SAMP.img", L"\\SAMP\\SAMP.ide", L"\\samp.saa",
        L"\\gtaweap3.ttf", L"\\sampaux3.ttf", L"\\sampgui.png",
        L"\\mouse.png",
        nullptr
    };
    for (const wchar_t** res = resources; *res; ++res) {
        std::wstring path = gtaDir + *res;
        DWORD attrs = GetFileAttributesW(path.c_str());
        Log("  resource %ls: %s", path.c_str(),
            attrs != INVALID_FILE_ATTRIBUTES ? "EXISTS" : "NOT FOUND");
    }

    // Also check in the bundled samp directory.
    std::wstring selfDir = SelfDir();
    if (!selfDir.empty()) {
        Log("  --- bundled resources ---");
        for (const wchar_t** res = resources; *res; ++res) {
            std::wstring path = selfDir + L"samp" + *res;
            DWORD attrs = GetFileAttributesW(path.c_str());
            Log("  bundled %ls: %s", path.c_str(),
                attrs != INVALID_FILE_ATTRIBUTES ? "EXISTS" : "NOT FOUND");
        }
    }

    // 5. VFS status.
    if (skr::vfs::IsInstalled()) {
        Log("  VFS: active");
        skr::vfs::LogTraceSummary();
    } else {
        Log("  VFS: not installed");
    }

    Log("=== END DIAGNOSTICS ===");
}
#endif // SKR_TEST_CERTIFICATION

} // namespace

bool PreloadSamp(const ConnectParams* startup) {
    std::lock_guard<std::mutex> lk(g_loadMutex);
    if (g_sampModule) return true;

    const int gameState = *reinterpret_cast<volatile int*>(kGtaGameState);
    if (gameState >= kGtaStateFrontendIdle) {
        // Pre-game patches would land on structures GTA already allocated.
        Log("preload refused: GTA already at gGameState=%d", gameState);
        return false;
    }

    // Search order:
    //   1. Bundled runtime beside SKR.Client.dll: <self_dir>\samp\samp.dll
    //   2. GTA installation directory: <gta_dir>\samp.dll (legacy / developer)
    // Nothing is installed into or written to the GTA directory.
    std::wstring sampPath;
    std::wstring selfDir = SelfDir();
    if (!selfDir.empty())
        sampPath = FindSampAt(selfDir + L"samp\\samp.dll");
    if (sampPath.empty()) {
        std::wstring dir = GtaDir();
        sampPath = FindSampAt(dir + L"\\samp.dll");
    }
    if (sampPath.empty()) {
        Log("samp.dll not found (tried bundled and GTA directory)");
        return false;
    }

    char** cmdLinePtr = FindAnsiCommandLinePointer();
    if (!cmdLinePtr || !*cmdLinePtr) {
        Log("samp.dll not loaded: GetCommandLineA has an unexpected shape");
        return false;
    }

    // Only -c: the server is chosen later in the lobby and written to the
    // Settings block before the gate opens. Kept alive for the process.
    static std::string sampCmdLine;
    sampCmdLine = *cmdLinePtr;
    sampCmdLine += " -c";

    Log("preloading samp from %ls (gGameState=%d)",
        sampPath.c_str(), gameState);

    char* originalCmdLine = *cmdLinePtr;

#ifdef SKR_TEST_CERTIFICATION
    // ── VFS: Install resource redirection BEFORE loading samp.dll ────
    // samp.dll's DllMain (with -c) may probe for resource files (SAMP.img,
    // fonts, etc.) relative to the GTA directory or its own directory.
    // The bundled SA-MP runtime files are in <self_dir>\samp\, not in the
    // GTA directory. The VFS intercepts file operations and redirects
    // verified SA-MP resource paths to the bundled location.
    {
        skr::vfs::VfsConfig vfsConfig;
        vfsConfig.sampResourceDir = selfDir + L"samp\\";
        vfsConfig.gtaDir = GtaDir();
        vfsConfig.traceAll = true;  // Full trace for first diagnostic run
        if (skr::vfs::Install(vfsConfig)) {
            Log("VFS installed: redirecting SA-MP resources from %ls to %ls",
                vfsConfig.gtaDir.c_str(), vfsConfig.sampResourceDir.c_str());
        } else {
            Log("VFS install FAILED: proceeding without resource redirection");
        }
    }
#endif // SKR_TEST_CERTIFICATION

    *cmdLinePtr = sampCmdLine.data();

    HMODULE module = LoadLibraryExW(
        sampPath.c_str(),
        nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
        LOAD_LIBRARY_SEARCH_DEFAULT_DIRS
    );

    // Capture immediately, before other API operations.
    const DWORD loadError = module ? ERROR_SUCCESS : GetLastError();

    *cmdLinePtr = originalCmdLine;

    if (!module) {
        Log("SA-MP LoadLibraryExW FAILED: error %lu (path=%ls)", loadError, sampPath.c_str());
        // Error 126 = ERROR_MOD_NOT_FOUND (a dependency DLL is missing)
        // Error 1114 = ERROR_DLL_INIT_FAILED (DllMain returned FALSE)
        if (loadError == 126) {
            Log("  hint: a dependency of samp.dll was not found in the DLL search path");
            Log("  searched: samp dir, System32");
        }
#ifdef SKR_TEST_CERTIFICATION
        if (loadError == 1114) {
            DiagnoseError1114(sampPath.c_str());
        }
        // Remove VFS hooks regardless of success or failure.
        skr::vfs::Remove();
#endif
        return false;
    }

    Log("SA-MP R5 loaded successfully: module=%p", module);

#ifdef SKR_TEST_CERTIFICATION
    // Log VFS trace summary after successful load, then keep hooks active
    // for continued SA-MP initialization (Samp_MainThread resource access).
    if (skr::vfs::IsInstalled()) {
        skr::vfs::LogTraceSummary();
        Log("VFS hooks remain active for SA-MP initialization phase");
    }
#endif

    if (!IsPinnedR5Image(module)) {
        Log("samp.dll rejected: PE signature is not the pinned R5 build");
        FreeLibrary(module);
#ifdef SKR_TEST_CERTIFICATION
        skr::vfs::Remove();
#endif
        return false;
    }

    auto* base = reinterpret_cast<uint8_t*>(module);
    const auto* settings = reinterpret_cast<const R5Settings*>(base + kR5SettingsRva);
    const void* sampGame = *reinterpret_cast<void* const*>(base + kR5SampGameRva);
    if (!settings->onlineGame || !sampGame) {
        Log("samp.dll did not open its client bootstrap (g_ModeClient=%d CGame=%p)",
            settings->onlineGame, sampGame);
#ifdef SKR_TEST_CERTIFICATION
        skr::vfs::Remove();
#endif
        return false;
    }

    if (startup) {
        if (!startup->serverAddress || !startup->playerName || !startup->serverPort ||
            !IsSafeArg(startup->serverAddress, true) || !IsSafeArg(startup->playerName, false)) return false;
        // GTA is suspended below state 7. Fill the verified Settings block
        // before its native bootstrap proceeds; retain native state waits.
        auto* selected = reinterpret_cast<R5Settings*>(base + kR5SettingsRva);
        strncpy_s(selected->host, startup->serverAddress, _TRUNCATE);
        _snprintf_s(selected->port, sizeof(selected->port), _TRUNCATE, "%u", startup->serverPort);
        strncpy_s(selected->nickname, startup->playerName, _TRUNCATE);
        g_sampModule = module;
        g_loaded.store(true);
        return true;
    }

    // Samp_MainThread (RVA 0xC3A80, initialization_flow.md §4) waits with
    // `cmp dword [gGameState], 7` before StartGame. Point both compares at
    // our gate so StartGame only runs after the player picks a server.
    // The thread cannot reach them yet: GTA is far from state 7.
    const uintptr_t gateAddr = reinterpret_cast<uintptr_t>(&g_startGate);

    for (uintptr_t rva : kStartGameCompareRvas) {
        uint8_t* insn = base + rva;
        if (memcmp(insn, kStartGameCompare, sizeof(kStartGameCompare)) != 0) {
            Log("samp start gate: unexpected bytes at RVA 0x%X, refusing", static_cast<unsigned>(rva));
            return false;
        }
    }

    for (uintptr_t rva : kStartGameCompareRvas) {
        uint8_t* operand = base + rva + 2;
        DWORD old = 0;
        VirtualProtect(operand, sizeof(uint32_t), PAGE_EXECUTE_READWRITE, &old);
        memcpy(operand, &gateAddr, sizeof(uint32_t));
        VirtualProtect(operand, sizeof(uint32_t), old, &old);
    }

    FlushInstructionCache(GetCurrentProcess(), base, kR5ImageSize);

    g_sampModule = module;
    Log("samp.dll preloaded at 0x%p, CGame=%p, StartGame held at the gate", module, sampGame);
    return true;
}

bool LoadSamp(const ConnectParams& params) {
    std::lock_guard<std::mutex> lk(g_loadMutex);

    if (g_loaded.load() || !g_sampModule || !params.serverAddress || !params.playerName ||
        !params.serverAddress[0] || !params.playerName[0] || params.serverPort == 0) {
        if (!g_sampModule) Log("connect refused: samp.dll was not preloaded at startup");
        return false;
    }

    if (!IsSafeArg(params.serverAddress, true) || !IsSafeArg(params.playerName, false)) {
        Log("connect refused: host or nickname contains unsupported characters");
        return false;
    }

    const int gameState = *reinterpret_cast<volatile int*>(kGtaGameState);
    if (gameState != kGtaStateFrontendIdle) {
        Log("connect refused: GTA not idle in the frontend (gGameState=%d)", gameState);
        return false;
    }

    // Samp_Init passes these to the CNetGame constructor (initialization_flow.md §7.1).
    auto* settings = reinterpret_cast<R5Settings*>(reinterpret_cast<uint8_t*>(g_sampModule) + kR5SettingsRva);
    strncpy_s(settings->host, params.serverAddress, _TRUNCATE);
    _snprintf_s(settings->port, sizeof(settings->port), _TRUNCATE, "%u", params.serverPort);
    strncpy_s(settings->nickname, params.playerName, _TRUNCATE);

    g_loaded.store(true);
    g_startGate.store(kGtaStateFrontendIdle);
    Log("connect staged for %s:%u, StartGame released", settings->host, params.serverPort);

#ifdef SKR_TEST_CERTIFICATION
    // Remove VFS hooks now that SA-MP initialization is complete and the
    // game is transitioning to gameplay. Resource files are memory-mapped
    // by this point.
    if (skr::vfs::IsInstalled()) {
        skr::vfs::Remove();
        Log("VFS removed: SA-MP initialization complete, resources loaded");
    }
#endif

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

    auto* base = reinterpret_cast<uint8_t*>(g_sampModule);
    void* netGame = *reinterpret_cast<void**>(base + kR5NetGameRva);
    if (!netGame) {
        Log("Connect deferred: CNetGame not created yet");
        return false;
    }
    using ConnectFn = void(__thiscall*)(void*);
    reinterpret_cast<ConnectFn>(base + kR5ConnectRva)(netGame);
    g_connectInvoked.store(true);
    Log("CNetGame::Connect invoked for the pinned R5 build");
    return true;
}

int TickConnection() {
    if (!g_loaded.load() || !g_sampModule) return -1;
    auto* base = reinterpret_cast<uint8_t*>(g_sampModule);
    void* netGame = *reinterpret_cast<void**>(base + kR5NetGameRva);
    if (!netGame) return -1;
    // Byte at +0x3CD (game_state.cpp); SA-MP connects on its own from WaitConnect (1).
    return *(reinterpret_cast<uint8_t*>(netGame) + 0x3CD);
}

} // namespace skr::samp_loader
