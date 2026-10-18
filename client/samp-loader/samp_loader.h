#pragma once
// Dynamic SA-MP loader — loads samp.dll at runtime when the user
// picks a server from the lobby, without restarting GTA.
//
// SA-MP must hook GTA functions at load time.  We call LoadLibrary
// which triggers DllMain → DLL_PROCESS_ATTACH, and SA-MP patches
// GTA code sections from there.  This works because GTA's game
// loop is already running and the code sections are live.
//
// NOTE: SA-MP needs the -c command-line flag to activate.  The
//       launcher writes this flag into the GTA process command
//       line BEFORE calling LoadSamp().

#include <cstdint>

namespace skr::samp_loader {

struct ConnectParams {
    const char* serverAddress;   // "127.0.0.1"
    uint16_t    serverPort;      // 7777
    const char* playerName;      // "Player"
};

// Load samp.dll from the GTA directory.  Returns true on success.
// May only be called once per process lifetime.
bool LoadSamp();

// True after LoadSamp() succeeded.
bool IsSampLoaded();

// Trigger SA-MP's internal connect to a server.
// Requires IsSampLoaded() == true.
// This writes to SA-MP's internal state (netGame) to initiate
// the RakNet connection — same as if the user clicked Connect
// in SA-MP's server browser.
bool Connect(const ConnectParams& params);

} // namespace skr::samp_loader
