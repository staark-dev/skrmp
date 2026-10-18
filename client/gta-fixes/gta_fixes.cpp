#include "gta-fixes/gta_fixes.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace skr::gta_fixes {

namespace {

void Log(const char* msg) {
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    char* slash = strrchr(path, '\\');
    if (!slash) return;
    snprintf(slash + 1, MAX_PATH - (slash + 1 - path), "SKR\\gta_fixes.log");
    FILE* f = fopen(path, "a");
    if (!f) return;
    fprintf(f, "[%lu] %s\n", GetTickCount(), msg);
    fclose(f);
}

// ── Radio stream: division by zero on a track with zero length ─────────
// In the CAEStreamThread code (just before CAEStreamThread::GetActiveTrackID
// 0x4F1560, plugin-sdk), GTA builds the track decoder, asks its length
// (vtbl[2]) and seeks to `startPos % length`. A track reporting length 0
// faults with #DE (seen as 0xC0000094 at 0x4F1464). Disassembly of the local
// gta_sa.exe 1.0 US:
//   004F145C  8B C8        mov  ecx, eax          ; length
//   004F145E  8B 44 24 1C  mov  eax, [esp+1Ch]    ; start position
//   004F1462  33 D2        xor  edx, edx
//   004F1464  F7 F1        div  ecx               ; edx = start % length
//   004F1466  8B CF        mov  ecx, edi          ; (continues: push edx; SetCursor)
// The cave repeats these and skips the div when the length is 0, so the
// track starts from 0 instead of crashing.
constexpr uintptr_t kRadioDivSite = 0x004F145C;
constexpr uintptr_t kRadioDivResume = 0x004F1466;
constexpr uint8_t kRadioDivOriginal[] = {
    0x8B, 0xC8, 0x8B, 0x44, 0x24, 0x1C, 0x33, 0xD2, 0xF7, 0xF1,
};

bool WriteCode(uintptr_t addr, const void* bytes, size_t len) {
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(addr), len, PAGE_EXECUTE_READWRITE, &old))
        return false;
    memcpy(reinterpret_cast<void*>(addr), bytes, len);
    VirtualProtect(reinterpret_cast<void*>(addr), len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(addr), len);
    return true;
}

int32_t Rel32(uintptr_t from, size_t instrLen, uintptr_t to) {
    return static_cast<int32_t>(to - (from + instrLen));
}

bool FixRadioZeroLengthDivide() {
    if (memcmp(reinterpret_cast<const void*>(kRadioDivSite), kRadioDivOriginal,
               sizeof(kRadioDivOriginal)) != 0) {
        Log("radio zero-length fix: unexpected bytes at 0x4F145C, skipped");
        return false;
    }

    auto* cave = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!cave) return false;

    size_t n = 0;
    const uint8_t prologue[] = {
        0x8B, 0xC8,              // mov  ecx, eax
        0x8B, 0x44, 0x24, 0x1C,  // mov  eax, [esp+1Ch]
        0x33, 0xD2,              // xor  edx, edx
        0x85, 0xC9,              // test ecx, ecx
        0x74, 0x02,              // jz   +2 (skip div: edx stays 0)
        0xF7, 0xF1,              // div  ecx
    };
    memcpy(cave, prologue, sizeof(prologue));
    n += sizeof(prologue);
    const uintptr_t jmpBack = reinterpret_cast<uintptr_t>(cave + n);
    cave[n++] = 0xE9;            // jmp kRadioDivResume
    const int32_t back = Rel32(jmpBack, 5, kRadioDivResume);
    memcpy(cave + n, &back, 4);
    n += 4;
    FlushInstructionCache(GetCurrentProcess(), cave, n);

    uint8_t site[sizeof(kRadioDivOriginal)];
    memset(site, 0x90, sizeof(site));   // nop padding after the jmp
    site[0] = 0xE9;
    const int32_t to = Rel32(kRadioDivSite, 5, reinterpret_cast<uintptr_t>(cave));
    memcpy(site + 1, &to, 4);
    if (!WriteCode(kRadioDivSite, site, sizeof(site))) {
        Log("radio zero-length fix: VirtualProtect failed");
        return false;
    }
    Log("radio zero-length fix: installed at 0x4F145C");
    return true;
}

} // namespace

int InstallGtaFixes() {
    int applied = 0;
    if (FixRadioZeroLengthDivide()) ++applied;
    return applied;
}

} // namespace skr::gta_fixes
