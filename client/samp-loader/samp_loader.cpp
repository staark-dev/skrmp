#include "samp-loader/samp_loader.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <atomic>
#include <array>
#include <cstdio>
#include <mutex>
#include <string>

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

} // namespace

bool PreloadSamp() {
    std::lock_guard<std::mutex> lk(g_loadMutex);
    if (g_sampModule) return true;

    const int gameState = *reinterpret_cast<volatile int*>(kGtaGameState);
    if (gameState >= kGtaStateFrontendIdle) {
        // Pre-game patches would land on structures GTA already allocated.
        Log("preload refused: GTA already at gGameState=%d", gameState);
        return false;
    }

    std::wstring dir = GtaDir();
    std::wstring sampPath = dir + L"\\samp.dll";
    std::wstring hidden = sampPath + L".skr";
    if (GetFileAttributesW(hidden.c_str()) != INVALID_FILE_ATTRIBUTES)
        MoveFileExW(hidden.c_str(), sampPath.c_str(), MOVEFILE_REPLACE_EXISTING);
    if (GetFileAttributesW(sampPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Log("samp.dll not found (tried restore from .skr and original)");
        return false;
    }
    if (!HashMatchesPinnedR5(sampPath.c_str())) {
        Log("samp.dll rejected: SHA-256 is not the pinned R5 build");
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

    Log("preloading samp from %ls (gGameState=%d)", sampPath.c_str(), gameState);
    char* originalCmdLine = *cmdLinePtr;
    *cmdLinePtr = sampCmdLine.data();
    HMODULE module = LoadLibraryW(sampPath.c_str());
    *cmdLinePtr = originalCmdLine;
    if (!module) {
        Log("LoadLibrary FAILED: error %lu", GetLastError());
        return false;
    }
    if (!IsPinnedR5Image(module)) {
        Log("samp.dll rejected: PE signature is not the pinned R5 build");
        FreeLibrary(module);
        return false;
    }

    auto* base = reinterpret_cast<uint8_t*>(module);
    const auto* settings = reinterpret_cast<const R5Settings*>(base + kR5SettingsRva);
    const void* sampGame = *reinterpret_cast<void* const*>(base + kR5SampGameRva);
    if (!settings->onlineGame || !sampGame) {
        Log("samp.dll did not open its client bootstrap (g_ModeClient=%d CGame=%p)",
            settings->onlineGame, sampGame);
        return false;
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
