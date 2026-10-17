#include "samp-hooks/samp_hooks.h"
#include <atomic>
#include <cstring>
#include <cstdio>

namespace skr::samp_hooks {

namespace {

// ── SA-MP 0.3.7-R5 RVAs (ImageBase 0x10000000) ─────────────────────
// From D:\SA-MP Reverse\RE\client\docs\14-catalog-functii.md
constexpr DWORD kRVA_CChat_AddMessage = 0x000680F0;  // CChat_AddMessage(CChat*, char* fmt, ...)
constexpr DWORD kRVA_g_Chat           = 0x0026EB80;  // CChat** global pointer
constexpr DWORD kRVA_g_NetGame        = 0x0026EB94;  // CNetGame** global pointer

// CNetGame state offset: +0x3CD = game state byte
// State 5 = in-game, 9 = class selection
constexpr DWORD kNetGameStateOffset = 0x3CD;

// GTA game state address (waits for value 7 before SA-MP initialises)
constexpr uintptr_t kGtaGameState = 0x00C8D4C0;

HMODULE     g_sampBase    = nullptr;
HANDLE      g_hookThread  = nullptr;
std::atomic<bool> g_hooked{false};
std::atomic<bool> g_shutdown{false};

// ── 5-byte JMP detour ───────────────────────────────────────────────
struct Detour {
    uintptr_t target   = 0;
    uint8_t   original[5]{};
    bool      installed = false;
};

bool WriteDetour(Detour& d, uintptr_t target, uintptr_t hook) {
    d.target = target;
    DWORD oldProt = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(target), 5, PAGE_EXECUTE_READWRITE, &oldProt))
        return false;
    memcpy(d.original, reinterpret_cast<void*>(target), 5);
    auto* code = reinterpret_cast<uint8_t*>(target);
    code[0] = 0xE9; // JMP rel32
    *reinterpret_cast<int32_t*>(code + 1) =
        static_cast<int32_t>(hook - target - 5);
    VirtualProtect(reinterpret_cast<void*>(target), 5, oldProt, &oldProt);
    d.installed = true;
    return true;
}

void RemoveDetour(Detour& d) {
    if (!d.installed) return;
    DWORD oldProt = 0;
    if (VirtualProtect(reinterpret_cast<void*>(d.target), 5, PAGE_EXECUTE_READWRITE, &oldProt)) {
        memcpy(reinterpret_cast<void*>(d.target), d.original, 5);
        VirtualProtect(reinterpret_cast<void*>(d.target), 5, oldProt, &oldProt);
    }
    d.installed = false;
}

// ── Chat filter ─────────────────────────────────────────────────────
// CChat_AddMessage is __cdecl: (CChat* pChat, char* fmt, ...)
// We intercept to suppress SA-MP branding messages.

Detour g_chatDetour;

// Trampoline: call the original 5 bytes then jump past them
// For simplicity we use a naked function with inline asm (MSVC x86)
using ChatAddMessageFn = void(__cdecl*)(void* pChat, const char* fmt, ...);
ChatAddMessageFn g_origChatAddMessage = nullptr;

// We can't easily make a generic trampoline for varargs, so instead
// we patch at a higher level: hook the AddMessage, check the text,
// and either call through or suppress.
// Since CChat_AddMessage is variadic, we hook the INTERNAL non-variadic
// display function instead: SAMP_DisplayGameText @ RVA 0x000A0CE0
// Signature: void __cdecl (char* text, DWORD color, int type)

constexpr DWORD kRVA_DisplayGameText = 0x000A0CE0;

Detour g_displayDetour;

using DisplayGameTextFn = void(__cdecl*)(const char* text, DWORD color, int type);
DisplayGameTextFn g_origDisplayGameText = nullptr;

// Trampoline buffer (executable memory) for original instructions
uint8_t* g_displayTrampoline = nullptr;

bool BuildTrampoline(const Detour& d, uint8_t*& trampoline) {
    trampoline = reinterpret_cast<uint8_t*>(
        VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) return false;
    memcpy(trampoline, d.original, 5);
    trampoline[5] = 0xE9; // JMP back to target+5
    *reinterpret_cast<int32_t*>(trampoline + 6) =
        static_cast<int32_t>((d.target + 5) - reinterpret_cast<uintptr_t>(trampoline + 10));
    return true;
}

// Patterns to suppress in chat/game text
static bool ShouldSuppressText(const char* text) {
    if (!text) return false;
    // SA-MP version branding
    if (strstr(text, "SA-MP")) return true;
    if (strstr(text, "SA:MP")) return true;
    if (strstr(text, "San Andreas Multiplayer")) return true;
    return false;
}

void __cdecl HookedDisplayGameText(const char* text, DWORD color, int type) {
    if (ShouldSuppressText(text)) return;
    g_origDisplayGameText(text, color, type);
}

// ── Also hook CChat_AddMessage for chat-line messages ───────────────
constexpr DWORD kRVA_ChatAddMsg = 0x000680F0;
Detour g_chatAddDetour;
uint8_t* g_chatAddTrampoline = nullptr;

// CChat_AddMessage signature from RE: (CChat*, int type, const char* text, ...)
// Actually from the code: it's called with format strings.
// We hook the non-variadic internal: offset +0x64010 is AddChatMessage
// Let's use a different approach: patch the chat message RPC handler.
// RPC 73 handler @ RVA 0x000198F0 — reads bitstream then calls DisplayGameText.
// We already hook DisplayGameText, so SA-MP's chat messages are filtered.

// For the direct AddMessage calls (local messages like "Connected to..."),
// hook CChat_AddMessage too. It's variadic but the first string arg after
// pChat contains the message pattern.

// Simpler approach: also hook the AddMessage wrapper.
// Signature: void CChat_AddMessage(CChat*, int type, const char* text,
//            const char* prefix, DWORD textColor, DWORD prefixColor)
// RE shows it at 0x10064010 — but let's verify. The 0x100680F0 entry in
// the catalog is the formatted version. Let's just use DisplayGameText
// which catches everything going to the game's text system.

// ── SA-MP splash suppression ────────────────────────────────────────
// SA-MP renders its splash/class-selection via ClassSel_Build @ RVA 0x000C5620.
// We don't need to hook it — our D3D9 intro overlay renders on top.
// The real issue: SA-MP's splash blocks input. We suppress by ensuring
// our intro finishes before SA-MP's class selection activates.

// ── Auto-spawn fix ──────────────────────────────────────────────────
// The player gets stuck in class selection because SA-MP expects the
// client to send a spawn request. We hook the per-frame tick to
// detect class-selection state and force a spawn request.
//
// SA-MP game states (CNetGame+0x3CD):
//   1 = connecting, 2 = connected, 5 = in-game, 9 = class selection
//
// When state == 9 (class selection), we need to send RPC "RequestSpawn"
// (ID 129 based on known SA-MP RPC tables) then "Spawn" (RPC 52).
// But it's simpler: write state 5 to skip class selection entirely,
// since the server already called SpawnPlayer() in OnPlayerConnect.

// Alternative: hook into SA-MP's frame tick and force spawn.
// The cleanest approach is to NOP the class-selection branch in
// SA-MP's per-frame processing. When CNetGame state == 9, SA-MP
// enters class selection mode. We patch the state check to skip it.

// From RE: NetGame_TickInGame runs when state == 5.
// The state dispatch is in SAMP_TickSubsystems (Samp_TickFrame).
// CNetGame+0x3CD is checked: if 5 → tick in-game, if 9 → class sel.

// Simplest fix: after SA-MP initialises, poll CNetGame state and
// force it to 5 when it's 9 (class selection).

void ForceSkipClassSelection() {
    if (!g_sampBase) return;
    auto** ppNetGame = reinterpret_cast<void**>(
        reinterpret_cast<uintptr_t>(g_sampBase) + kRVA_g_NetGame);
    if (!*ppNetGame) return;

    auto* pState = reinterpret_cast<uint8_t*>(
        reinterpret_cast<uintptr_t>(*ppNetGame) + kNetGameStateOffset);

    // State 9 = class selection → force to 5 (in-game)
    if (*pState == 9) {
        *pState = 5;
    }
}

// ── Hook installer thread ───────────────────────────────────────────

DWORD WINAPI SampHookThread(LPVOID) {
    // Wait for samp.dll to load (it's injected before us, but might
    // not have finished DllMain yet)
    for (int i = 0; i < 600 && !g_shutdown.load(); ++i) {
        g_sampBase = GetModuleHandleA("samp.dll");
        if (g_sampBase) break;
        Sleep(50);
    }
    if (!g_sampBase) return 1;

    // Wait for SA-MP to finish installing its hooks (game state >= 8)
    auto* pGameState = reinterpret_cast<volatile DWORD*>(kGtaGameState);
    for (int i = 0; i < 600 && !g_shutdown.load(); ++i) {
        if (*pGameState >= 8) break;
        Sleep(50);
    }

    // Wait a bit more for SA-MP subsystems to initialise
    Sleep(500);

    auto base = reinterpret_cast<uintptr_t>(g_sampBase);

    // Hook SAMP_DisplayGameText to filter branded messages
    uintptr_t displayAddr = base + kRVA_DisplayGameText;
    WriteDetour(g_displayDetour, displayAddr,
                reinterpret_cast<uintptr_t>(&HookedDisplayGameText));
    if (g_displayDetour.installed) {
        BuildTrampoline(g_displayDetour, g_displayTrampoline);
        g_origDisplayGameText = reinterpret_cast<DisplayGameTextFn>(g_displayTrampoline);
    }

    g_hooked.store(true);

    // Run a tick loop to force-skip class selection whenever it appears
    while (!g_shutdown.load()) {
        ForceSkipClassSelection();
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
    RemoveDetour(g_displayDetour);
    if (g_displayTrampoline) {
        VirtualFree(g_displayTrampoline, 0, MEM_RELEASE);
        g_displayTrampoline = nullptr;
    }
    g_hooked.store(false);
}

} // namespace skr::samp_hooks
