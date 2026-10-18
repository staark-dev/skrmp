#pragma once
#include <cstdint>

// SKR session-info RPC: SKR Gate (open.mp component) -> SKRClient.dll.
//
// Tells the client how the server takes over from the SKR lobby once the
// player is in the world. RPC ID 241 is SKR-private, chosen like 240
// (client_state.h): above open.mp NetCode's IDs (<= 185) and SA-MP R5's
// client registrations (11..178, D:\SA-MP Reverse network_protocol.md §4).
//
// Payload v1 (RakNet bitstream, byte-aligned):
//   u8 version = kSessionInfoVersion
//   u8 mode    = SessionMode
//
// The mode comes from the scripts: when the gamemode (or a filterscript)
// implements `public OnGameModeIntro()` and returns non-zero, the
// session is CefIntro; otherwise None. The gate asks on client init, after
// the scripts' OnPlayerConnect, and sends the result.

namespace skr::protocol {

constexpr uint8_t kRpcSessionInfo = 241;
constexpr uint8_t kSessionInfoVersion = 1;

enum class SessionMode : uint8_t {
    None = 0,      // no server UI: the lobby hides as soon as the player is in the world
    CefIntro = 1,  // the server shows its own CEF intro/login: the lobby waits for it
};

constexpr const char* kIntroCallback = "OnGameModeIntro";

} // namespace skr::protocol
