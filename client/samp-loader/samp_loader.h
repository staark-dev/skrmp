#pragma once
// SA-MP loader for the pinned 0.3.7-R5 binary: preloaded at process start,
// game start held until the player picks a server in the lobby.

#include <cstdint>

namespace skr::samp_loader {

struct ConnectParams {
    const char* serverAddress;   // "127.0.0.1"
    uint16_t    serverPort;      // 7777
    const char* playerName;      // "Player"
};

// Load the pinned samp.dll before GTA initializes (its pre-game patches must
// precede GTA's own allocations) with SA-MP's client mode on, and hold its
// StartGame at a gate. Returns false for any unknown binary or late call.
bool PreloadSamp(const ConnectParams* startup = nullptr);

// Write the chosen server into SA-MP's Settings block and open the gate:
// SA-MP starts the game and creates CNetGame for this server. Once per process.
bool LoadSamp(const ConnectParams& params);

// True after LoadSamp() succeeded.
bool IsSampLoaded();

// Trigger a subsequent attempt after the native bootstrap created CNetGame.
bool Connect(const ConnectParams& params);

// Called from the game frame after LoadSamp. Starts one explicit attempt if
// SA-MP has created CNetGame, then only observes its state.
// Returns -1 while CNetGame does not exist, otherwise the native game state.
int TickConnection();

} // namespace skr::samp_loader
