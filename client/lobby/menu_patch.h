#pragma once
#include <cstdint>

// GTA SA 1.0 US main-menu neutralisation.
//
// Provenance (DK22Pac/plugin-sdk, GAME_10US_COMPACT meta files, confirmed
// against gta-reversed-modern/source/game_sa/Menu/CMenuManager.cpp):
//   FrontEndMenuManager singleton         = 0x00BA6748
//   CMenuManager::Process()      __thiscall @ 0x0057B440   (void())
//   CMenuManager::DrawFrontEnd() __thiscall @ 0x0057B210   (void())
//
// Both methods take only `this` in ECX on x86 __thiscall and return void
// with no stack args, so overwriting the first byte with 0xC3 (near ret)
// gives a clean return to the caller. Phase 1 live test showed the previous
// strict prologue allowlist refused to patch — the diagnostic struct below
// now reports the actual first byte so the exact mismatch is visible.

namespace skr::lobby {

struct MenuPatchStatus {
    struct Entry {
        const char* name;
        uintptr_t   addr;
        uint8_t     firstByte;      // byte seen at addr before any write
        bool        patched;        // true if the site is now 0xC3
        bool        alreadyPatched; // 0xC3 was already there (we didn't write)
        const char* reason;         // nullptr on success; else short English reason
    };
    Entry entries[2];
    int   count;
};

// Returns true when every site is now 0xC3 (patched or already-patched).
// Writes a per-site report into `out`. Call once per frame until true; the
// caller is expected to log on first success / first permanent failure.
bool PatchGtaMainMenu(MenuPatchStatus& out);

// Reverts any patch we made this process (sites that were already 0xC3 at
// startup stay as they were).
void RestoreGtaMainMenu();

} // namespace skr::lobby
