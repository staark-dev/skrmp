#pragma once
#include <cstdint>

// GTA SA 1.0 US main-menu neutralisation.
//
// Canonical Hoodlum profile evidence (SHA-256 A559AA77...45CBD26):
//   FrontEndMenuManager singleton         = 0x00BA6748
//   CMenuManager::Process()      __thiscall @ 0x0057B440   (void())
//
// 0x0057B210 is inside an instruction in the preceding routine, not a
// DrawFrontEnd entry. It must never be patched. Process is suppressed with a
// one-byte near return only after an exact profile signature and executable
// section validation succeed.

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
    Entry entries[1];
    int   count;
};

// Returns true when the canonical Process site is now 0xC3.
// Writes a per-site report into `out`. Call once per frame until true; the
// caller is expected to log on first success / first permanent failure.
bool PatchGtaMainMenu(MenuPatchStatus& out);

// Reverts any patch we made this process (sites that were already 0xC3 at
// startup stay as they were).
void RestoreGtaMainMenu();

} // namespace skr::lobby
