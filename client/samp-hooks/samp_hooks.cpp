#include "samp-hooks/samp_hooks.h"
#include "game-state/game_state.h"
#include "render-d3d9/chat_box.h"
#include "skrx/skrx_vehicles_client.h"
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
std::atomic<skr::bootstrap::ComponentState> g_hookState{
    skr::bootstrap::ComponentState::NOT_STARTED};

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

    // Connection messages — rebrand for SKR client
    {"Connecting to %s:%d...",         "Connecting to %s:%d..."},
    {"Connected. Joining the game...", "Connected. Joining..."},

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

    // Chat timestamp "14:32" instead of "[14:32:05]"; the second literal is only used
    // by Chat_MeasureLayoutMetrics to size the timestamp column (chat.md §6.3).
    {"[%H:%M:%S]",             "%H:%M"},
    {"[19:58:34]",             "19:58"},

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

// ── SKR client signature for server-side verification ───────────────
// The server uses sendClientCheck(type=2) to read this address.
// Non-SKR clients won't have this value, so they get kicked.
constexpr uintptr_t kSkrSignatureAddr  = 0x00B7CE50;
constexpr uint32_t  kSkrSignatureValue = 0x00524B53; // "SKR\0"

static bool WriteSkrSignature() {
    DWORD oldProt = 0;
    auto* addr = reinterpret_cast<uint32_t*>(kSkrSignatureAddr);
    if (VirtualProtect(addr, 4, PAGE_READWRITE, &oldProt)) {
        *addr = kSkrSignatureValue;
        VirtualProtect(addr, 4, oldProt, &oldProt);
        return true;
    }
    return false;
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

// ── Chat line length ────────────────────────────────────────────────
// Chat_AddEntry (RVA 0x67BE0) wraps player messages longer than 0x48 chars into two
// entries; the text buffer is 0x90 bytes, so raising the threshold to 0x7F is safe
// (longer text still wraps through the original 0x48 split). Text is drawn with
// DT_NOCLIP, so Chat_Render's right margin needs no change. Disassembled from samp.dll R5.
static bool PatchBytes(uintptr_t addr, const uint8_t* expect, size_t expectLen,
                       size_t patchOff, const void* data, size_t len) {
    if (memcmp(reinterpret_cast<void*>(addr), expect, expectLen) != 0) return false;
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(addr + patchOff), len, PAGE_EXECUTE_READWRITE, &old))
        return false;
    memcpy(reinterpret_cast<void*>(addr + patchOff), data, len);
    VirtualProtect(reinterpret_cast<void*>(addr + patchOff), len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(addr), expectLen);
    return true;
}

static bool PatchChatLineLength(uintptr_t base) {
    static const uint8_t kCmpWrap[] = {0x83, 0xF8, 0x48};             // cmp eax, 0x48
    const uint8_t wrap = 0x7F;
    return PatchBytes(base + 0x67CA9, kCmpWrap, sizeof(kCmpWrap), 2, &wrap, 1);
}

// CChat (g_Chat at RVA 0x26EB80): +0xC m_bTimeStamp, +0x63DA m_bNeedsRedraw (chat.md §2).
constexpr DWORD kRVA_g_Chat = 0x0026EB80;

static bool EnableChatTimestamps() {
    if (!g_sampBase) return false;
    auto base = reinterpret_cast<uintptr_t>(g_sampBase);
    auto* chat = *reinterpret_cast<uint8_t**>(base + kRVA_g_Chat);
    if (!chat) return false;
    chat[0xC] = 1;
    *reinterpret_cast<uint32_t*>(chat + 0x63DA) = 1;
    return true;
}

// ── Hook installer thread ───────────────────────────────────────────
DWORD WINAPI SampHookThread(LPVOID) {
    g_hookState.store(skr::bootstrap::ComponentState::WAITING, std::memory_order_release);
    // Wait for samp.dll
    for (int i = 0; i < 600 && !g_shutdown.load(); ++i) {
        g_sampBase = GetModuleHandleA("samp.dll");
        if (g_sampBase) break;
        Sleep(50);
    }
    if (!g_sampBase) {
        g_hookState.store(skr::bootstrap::ComponentState::FAILED, std::memory_order_release);
        return 1;
    }

    // Patch format strings immediately (before SA-MP uses them)
    size_t imageSize = GetModuleImageSize(g_sampBase);
    uintptr_t sampAddr = reinterpret_cast<uintptr_t>(g_sampBase);
    if (imageSize > 0) {
        ScanAndPatchStrings(sampAddr, imageSize);
        if (!PatchChatLineLength(sampAddr)) {
            g_hookState.store(skr::bootstrap::ComponentState::FAILED, std::memory_order_release);
            return 1;
        }
    } else {
        g_hookState.store(skr::bootstrap::ComponentState::FAILED, std::memory_order_release);
        return 1;
    }

    // Write SKR signature so the server can identify us via sendClientCheck
    if (!WriteSkrSignature()) {
        g_hookState.store(skr::bootstrap::ComponentState::FAILED, std::memory_order_release);
        return 1;
    }

    // Wait for SA-MP to finish installing its hooks (game state >= 8)
    auto* pGameState = reinterpret_cast<volatile DWORD*>(kGtaGameState);
    bool sampReady = false;
    for (int i = 0; i < 600 && !g_shutdown.load(); ++i) {
        if (*pGameState >= 8) { sampReady = true; break; }
        Sleep(50);
    }
    if (!sampReady) {
        g_hookState.store(skr::bootstrap::ComponentState::FAILED, std::memory_order_release);
        return 1;
    }

    // Wait for SA-MP subsystems (CNetGame creation)
    Sleep(500);

    // Patch CNetGame session name once
    PatchNetGameSessionName();

    // Per-frame game-thread tick (chains whatever is already at the call site)
    const bool processHook = skr::game_state::InstallProcessHook();

    // Radar DrawMap inside CHud::Draw (RenderWare 2D state, not EndScene)
    const bool hudHook = skr::game_state::InstallHudDrawHook();

    // Panel behind the native SA-MP chat (verified call site, skipped if bytes differ)
    const bool chatHook = skr::render::InstallChatBoxHook();
    if (!processHook || !hudHook || !chatHook) {
        g_hookState.store(skr::bootstrap::ComponentState::FAILED, std::memory_order_release);
        return 1;
    }

    // SKR-X VEHICLES: hook CVehiclePool::Create for proxy→custom model swap.
    skr::skrx::vehicles::Init();
    skr::skrx::vehicles::InstallVehiclePoolHook(sampAddr);

    g_hookState.store(skr::bootstrap::ComponentState::INSTALLED, std::memory_order_release);

    // Tick loop: re-patch session name periodically
    bool timestampsOn = false;
    while (!g_shutdown.load()) {
        PatchNetGameSessionName();
        if (!timestampsOn) timestampsOn = EnableChatTimestamps();
        Sleep(100);
    }

    return 0;
}

} // anonymous namespace

bool InstallSampHooks() {
    if (g_hookThread) return false;
    g_shutdown.store(false, std::memory_order_release);
    g_hookState.store(skr::bootstrap::ComponentState::WAITING, std::memory_order_release);
    g_hookThread = CreateThread(nullptr, 0, SampHookThread, nullptr, 0, nullptr);
    if (!g_hookThread)
        g_hookState.store(skr::bootstrap::ComponentState::FAILED, std::memory_order_release);
    return g_hookThread != nullptr;
}

skr::bootstrap::ComponentState GetSampHookState() noexcept {
    return g_hookState.load(std::memory_order_acquire);
}

void RemoveSampHooks() {
    g_shutdown.store(true);
    if (g_hookThread) {
        WaitForSingleObject(g_hookThread, 5000);
        CloseHandle(g_hookThread);
        g_hookThread = nullptr;
    }
    g_sampBase = nullptr;
    g_hookState.store(skr::bootstrap::ComponentState::NOT_STARTED, std::memory_order_release);
}

} // namespace skr::samp_hooks
