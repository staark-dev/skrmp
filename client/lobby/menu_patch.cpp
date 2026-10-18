#include "lobby/menu_patch.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>

namespace skr::lobby {

namespace {

struct Site {
    uintptr_t addr;
    uint8_t   original;   // saved before patching; 0 if unpatched
    bool      patched;
    const char* name;
};

// Order matters only for logging; both sites are patched independently.
Site g_sites[] = {
    { 0x0057B440, 0, false, "CMenuManager::Process" },
    { 0x0057B210, 0, false, "CMenuManager::DrawFrontEnd" },
};

// Accept common x86 function-prologue first bytes. Refusing to patch anything
// else keeps a mismatched GTA build from being clobbered silently.
bool IsLikelyPrologue(uint8_t b) {
    return b == 0x55   // push ebp
        || b == 0x56   // push esi
        || b == 0x53   // push ebx
        || b == 0x57   // push edi
        || b == 0x83   // sub esp, imm8  / and esp, imm8
        || b == 0x81   // sub esp, imm32
        || b == 0x8B   // mov e_x, _
        || b == 0xA1;  // mov eax, [imm32]
}

bool WriteByte(uintptr_t addr, uint8_t value) {
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(addr), 1, PAGE_EXECUTE_READWRITE, &old))
        return false;
    *reinterpret_cast<uint8_t*>(addr) = value;
    VirtualProtect(reinterpret_cast<void*>(addr), 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(addr), 1);
    return true;
}

} // namespace

bool PatchGtaMainMenu() {
    bool allOk = true;
    for (auto& s : g_sites) {
        if (s.patched) continue;
        uint8_t first = *reinterpret_cast<uint8_t*>(s.addr);
        if (first == 0xC3) { s.patched = true; s.original = 0xC3; continue; }
        if (!IsLikelyPrologue(first)) { allOk = false; continue; }
        s.original = first;
        if (!WriteByte(s.addr, 0xC3)) { allOk = false; continue; }
        s.patched = true;
    }
    return allOk;
}

void RestoreGtaMainMenu() {
    for (auto& s : g_sites) {
        if (!s.patched || s.original == 0xC3) { s.patched = false; continue; }
        WriteByte(s.addr, s.original);
        s.patched = false;
        s.original = 0;
    }
}

} // namespace skr::lobby
