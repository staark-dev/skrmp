#include "samp-hooks/samp_hooks.h"
#include <atomic>
#include <cstring>

namespace skr::samp_hooks {

namespace {

// ── SA-MP 0.3.7-R5 RVAs (ImageBase 0x10000000) ─────────────────────
constexpr DWORD kRVA_g_NetGame = 0x0026EB94;
constexpr DWORD kNetGameStateOffset = 0x3CD;
constexpr uintptr_t kGtaGameState = 0x00C8D4C0;

HMODULE     g_sampBase   = nullptr;
HANDLE      g_hookThread = nullptr;
std::atomic<bool> g_shutdown{false};

// ── String patching ─────────────────────────────────────────────────
// Scan samp.dll's loaded image for exact ASCII strings and replace them
// in-place. The replacement must be <= original length (padded with \0).

struct StringPatch {
    const char* find;
    const char* replace;
};

static constexpr StringPatch kPatches[] = {
    // The actual "Started" string includes SA-MP color codes:
    // {FFFFFF}SA-MP {B9C9BF}0.3.7-R5 {FFFFFF}Started
    {"{FFFFFF}SA-MP {B9C9BF}0.3.7-R5 {FFFFFF}Started",
     "{FFFFFF}SKR {B9C9BF}Client R1 {FFFFFF}Started"},

    // Connection messages
    {"Connecting to %s:%d...", "Connecting..."},
    {"Connected. Joining the game...", "Joining server..."},

    // SA-MP 0.3.7-R5 (plain, used in various places)
    {"SA-MP 0.3.7-R5",         "SKR Client"},

    // Screenshot filenames and message
    {"\\screens\\sa-mp-%03i.png",        "\\screens\\skr-%03i.png"},
    {"Screenshot Taken - sa-mp-%03i.png", "Screenshot Taken - skr-%03i.png"},

    // Connection rejected messages
    {"CONNECTION REJECTED: Unable to allocate a player slot.",
     "Connection rejected: server full."},
    {"CONNECTION REJECTED: Bad mod version.",
     "Connection rejected: version mismatch."},
    {"CONNECTION REJECTED: Unacceptable NickName",
     "Connection rejected: invalid nickname."},
    {"CONNECTION REJECTED: Incorrect Version.",
     "Connection rejected: version mismatch."},

    // Window title
    {"GTA:SA:MP",              "GTA:SKR"},
};

static void PatchString(uintptr_t addr, size_t maxLen,
                         const char* replacement) {
    size_t repLen = strlen(replacement);
    if (repLen > maxLen) repLen = maxLen;

    DWORD oldProt = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(addr), maxLen,
                        PAGE_READWRITE, &oldProt))
        return;
    memcpy(reinterpret_cast<void*>(addr), replacement, repLen);
    // Zero-fill remainder so the old tail doesn't leak
    if (repLen < maxLen)
        memset(reinterpret_cast<void*>(addr + repLen), 0, maxLen - repLen);
    VirtualProtect(reinterpret_cast<void*>(addr), maxLen, oldProt, &oldProt);
}

static void ScanAndPatchStrings(uintptr_t base, size_t imageSize) {
    for (const auto& p : kPatches) {
        size_t findLen = strlen(p.find);
        if (findLen == 0 || findLen >= imageSize) continue;

        // Scan the entire image for the string (may appear more than once)
        auto* start = reinterpret_cast<const uint8_t*>(base);
        for (size_t off = 0; off + findLen < imageSize; ++off) {
            if (memcmp(start + off, p.find, findLen + 1) == 0) {
                PatchString(base + off, findLen, p.replace);
            }
        }
    }
}

static size_t GetModuleImageSize(HMODULE mod) {
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(mod);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        reinterpret_cast<uintptr_t>(mod) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    return nt->OptionalHeader.SizeOfImage;
}

// ── Also patch the "SA-MP" init string in CNetGame ──────────────────
// CNetGame+0x131 is initialised to "SA-MP" by the constructor.
// We poll and overwrite it once CNetGame is created.
static void PatchNetGameSessionName() {
    if (!g_sampBase) return;
    auto base = reinterpret_cast<uintptr_t>(g_sampBase);
    auto** ppNetGame = reinterpret_cast<void**>(base + kRVA_g_NetGame);
    if (!*ppNetGame) return;

    auto* sessionName = reinterpret_cast<char*>(
        reinterpret_cast<uintptr_t>(*ppNetGame) + 0x131);

    // Only patch if it still says "SA-MP"
    if (memcmp(sessionName, "SA-MP", 5) == 0) {
        DWORD oldProt = 0;
        VirtualProtect(sessionName, 0x100, PAGE_READWRITE, &oldProt);
        strncpy(sessionName, "SKR", 0x100);
        sessionName[0xFF] = '\0';
        VirtualProtect(sessionName, 0x100, oldProt, &oldProt);
    }
}

// ── Auto-spawn (force state 5 when class selection state 9) ─────────
static void ForceSkipClassSelection() {
    if (!g_sampBase) return;
    auto base = reinterpret_cast<uintptr_t>(g_sampBase);
    auto** ppNetGame = reinterpret_cast<void**>(base + kRVA_g_NetGame);
    if (!*ppNetGame) return;

    auto* pState = reinterpret_cast<uint8_t*>(
        reinterpret_cast<uintptr_t>(*ppNetGame) + kNetGameStateOffset);

    if (*pState == 9)
        *pState = 5;
}

// ── Hook installer thread ───────────────────────────────────────────
DWORD WINAPI SampHookThread(LPVOID) {
    // Wait for samp.dll
    for (int i = 0; i < 600 && !g_shutdown.load(); ++i) {
        g_sampBase = GetModuleHandleA("samp.dll");
        if (g_sampBase) break;
        Sleep(50);
    }
    if (!g_sampBase) return 1;

    // Patch format strings immediately (before SA-MP uses them)
    size_t imageSize = GetModuleImageSize(g_sampBase);
    if (imageSize > 0)
        ScanAndPatchStrings(reinterpret_cast<uintptr_t>(g_sampBase), imageSize);

    // Wait for SA-MP to finish installing its hooks (game state >= 8)
    auto* pGameState = reinterpret_cast<volatile DWORD*>(kGtaGameState);
    for (int i = 0; i < 600 && !g_shutdown.load(); ++i) {
        if (*pGameState >= 8) break;
        Sleep(50);
    }

    // Wait for SA-MP subsystems (CNetGame creation)
    Sleep(500);

    // Patch CNetGame session name once
    PatchNetGameSessionName();

    // Tick loop: force-skip class selection + re-patch session name
    while (!g_shutdown.load()) {
        ForceSkipClassSelection();
        PatchNetGameSessionName();
        Sleep(100);
    }

    return 0;
}

} // anonymous namespace

bool InstallSampHooks() {
    g_hookThread = CreateThread(nullptr, 0, SampHookThread, nullptr, 0, nullptr);
    return g_hookThread != nullptr;
}

void RemoveSampHooks() {
    g_shutdown.store(true);
    if (g_hookThread) {
        WaitForSingleObject(g_hookThread, 5000);
        CloseHandle(g_hookThread);
        g_hookThread = nullptr;
    }
}

} // namespace skr::samp_hooks
