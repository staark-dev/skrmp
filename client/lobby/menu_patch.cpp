#include "lobby/menu_patch.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace skr::lobby {

namespace {

struct Site {
    uintptr_t   addr;
    const char* name;
    uint8_t     original;        // byte captured on first-ever patch
    bool        originalCaptured;
    bool        patched;         // this process wrote the 0xC3
    bool        alreadyPatched;  // 0xC3 was already there at first touch
};

// GTA SA 1.0 US (HOODLUM standard SA-MP install). Both are __thiscall void().
Site g_sites[] = {
    { 0x0057B440, "CMenuManager::Process",      0, false, false, false },
    { 0x0057B210, "CMenuManager::DrawFrontEnd", 0, false, false, false },
};

// Reject only values that would indicate "nothing live here": INT3 debug fill,
// zero page, 0xFF padding. Everything else is treated as a real first byte —
// the previous allowlist refused real prologues such as 0x6A (push imm8) or
// 0xA0 (mov al, moff32) that CMenuManager::Process actually starts with on
// some builds. The caller logs the exact byte so a wrong offset is still
// obvious.
bool LooksLiveCode(uint8_t b) {
    return b != 0x00 && b != 0xCC && b != 0xFF;
}

bool WriteByte(uintptr_t addr, uint8_t value) {
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(addr), 1, PAGE_EXECUTE_READWRITE, &old))
        return false;
    *reinterpret_cast<uint8_t*>(addr) = value;
    DWORD restore = 0;
    VirtualProtect(reinterpret_cast<void*>(addr), 1, old, &restore);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(addr), 1);
    return true;
}

// Confirms the address lives in a mapped, executable region before we touch it.
bool IsAddrInProcessCode(uintptr_t addr) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    DWORD exec = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & exec) != 0;
}

} // namespace

bool PatchGtaMainMenu(MenuPatchStatus& out) {
    out.count = 0;
    bool allOk = true;
    for (auto& s : g_sites) {
        auto& e = out.entries[out.count++];
        e.name  = s.name;
        e.addr  = s.addr;
        e.firstByte = 0;
        e.patched = false;
        e.alreadyPatched = s.alreadyPatched;
        e.reason = nullptr;

        if (!IsAddrInProcessCode(s.addr)) {
            e.reason = "address not in executable mapping";
            allOk = false;
            continue;
        }

        uint8_t first = *reinterpret_cast<uint8_t*>(s.addr);
        e.firstByte = first;

        if (s.patched || first == 0xC3) {
            // Either we wrote it already, or something else did (or already-ret).
            if (!s.patched) { s.alreadyPatched = true; e.alreadyPatched = true; }
            s.patched = true;
            e.patched = true;
            continue;
        }
        if (!LooksLiveCode(first)) {
            e.reason = "first byte is 0x00/0xCC/0xFF (not live code)";
            allOk = false;
            continue;
        }

        if (!s.originalCaptured) { s.original = first; s.originalCaptured = true; }
        if (!WriteByte(s.addr, 0xC3)) {
            e.reason = "VirtualProtect/WriteByte failed";
            allOk = false;
            continue;
        }
        s.patched = true;
        e.patched = true;
    }
    return allOk;
}

void RestoreGtaMainMenu() {
    for (auto& s : g_sites) {
        if (s.patched && s.originalCaptured && !s.alreadyPatched) {
            WriteByte(s.addr, s.original);
        }
        s.patched = false;
    }
}

} // namespace skr::lobby
