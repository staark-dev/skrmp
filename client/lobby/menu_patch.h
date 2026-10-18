#pragma once

// GTA SA 1.0 US main-menu neutralisation.
//
// Provenance (DK22Pac/plugin-sdk, GAME_10US_COMPACT meta files, confirmed against
// gta-reversed-modern/source/game_sa/Menu/CMenuManager.cpp):
//   FrontEndMenuManager singleton         = 0x00BA6748
//   CMenuManager::Process()      __thiscall @ 0x0057B440   (void())
//   CMenuManager::DrawFrontEnd() __thiscall @ 0x0057B210   (void())
//
// Both methods take only `this` in ECX on x86 __thiscall and return void with no
// stack args, so overwriting the first byte with 0xC3 (near ret) gives a clean
// return to the caller. This is used by several GTA SA mods (e.g. Open Limit
// Adjuster, menu-replacement ASIs) and is reversible via RestoreGtaMainMenu().
//
// We only patch sites whose expected first byte matches a known function prologue
// (0x55 push ebp, or 0x83 sub esp, or 0x8B mov) so a different build/version is
// never clobbered silently.

namespace skr::lobby {

// Patches CMenuManager::Process and CMenuManager::DrawFrontEnd to return
// immediately. Safe to call once GTA SA's text is loaded (post state >= 8).
// Returns true on success (both sites patched).
bool PatchGtaMainMenu();

// Reverts the patch (restores the original first byte at each site).
void RestoreGtaMainMenu();

} // namespace skr::lobby
