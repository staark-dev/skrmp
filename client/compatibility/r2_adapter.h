#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>

// R2 Compatibility Adapter for samp-cef
//
// samp-cef at commit f14fe4fa does not support SA:MP 0.3.7-R2.
// It detects samp.dll file version (3,7,1) as Unknown and refuses to initialize.
//
// The samp-client-api crate (d497ffad) reads file version via GetFileVersionInfoA
// and maps: (3,7,0)->R1, (3,7,2)->R3, (3,8,0)->DL. R2 (3,7,1) falls through.
//
// Strategy: SKR.Client.dll loads before cef.asi. We patch the version detection
// result in samp-client-api after cef-client.dll is loaded, by scanning for the
// version enum storage and forcing it to a compatible value. This requires:
//
// 1. Known pattern in the cef-client.dll binary to locate the version enum.
// 2. R2-specific offsets for the samp structures samp-cef uses.
//
// R2 offsets differ from both R1 and R3. The following must be verified on
// the actual R2 samp.dll binary (SHA-256 to be recorded):
//
//   CNETGAME, CINPUT, CGAME, CDIALOG — global pointers in samp.dll
//   CNetGame struct layout (ip, hostname, port, gamestate, pools offsets)
//   CPlayerPool, CObjectPool layouts
//   WndProc hook target
//   Input/cursor functions
//
// NONE of the offsets below are verified. They are placeholders derived from
// community documentation and MUST be validated against the exact R2 binary.
// Do NOT ship with unverified offsets.

namespace skr::compat {

struct R2Offsets {
    // samp.dll global pointers (RVA from samp.dll base)
    uintptr_t CNETGAME;
    uintptr_t CINPUT;
    uintptr_t CGAME;
    uintptr_t CDIALOG;
    uintptr_t CGAME_SETCURSORMODE;
    uintptr_t CGAME_PROCESSINPUTENABLING;
    uintptr_t CDEATHWINDOW_DRAW;
};

// Unverified R2 placeholders — DO NOT USE without validation
constexpr R2Offsets kR2Offsets_UNVERIFIED = {
    .CNETGAME                    = 0x21A0F8,  // PLACEHOLDER — same as R1, likely different
    .CINPUT                      = 0x21A0E8,  // PLACEHOLDER
    .CGAME                       = 0x21A10C,  // PLACEHOLDER
    .CDIALOG                     = 0x21A0B8,  // PLACEHOLDER
    .CGAME_SETCURSORMODE         = 0x9BD30,   // PLACEHOLDER
    .CGAME_PROCESSINPUTENABLING  = 0x9BC10,   // PLACEHOLDER
    .CDEATHWINDOW_DRAW           = 0x66640,   // PLACEHOLDER
};

enum class SampVersion : uint8_t {
    Unknown,
    V037R1,
    V037R2,
    V037R3,
    V03DL
};

// Detect the SA:MP client version from samp.dll file version info.
SampVersion DetectSampVersion();

// Get the base address of samp.dll (null if not loaded).
HMODULE GetSampBase();

// Verify that the loaded samp.dll matches an expected R2 hash.
bool VerifyR2Binary(const uint8_t expectedSha256[32]);

// Check if samp-cef's cef-client.dll is loaded.
bool IsSampCefLoaded();

} // namespace skr::compat
