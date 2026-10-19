#pragma once
#include <cstdint>

// SKR radar-rect RPC: SKRClient.dll -> SKR Gate (open.mp component).
//
// The client reports the radar's screen-space rectangle (in pixels, origin
// top-left) whenever it changes — after SA-MP's aspect correction and the
// server's SKR_SetRadarSize scale.  The server stores it as PVars and fires
// OnPlayerRadarRect so scripts can align CEF HUD elements exactly.
//
// RPC ID 243 is SKR-private, chosen like 240–242.
//
// Payload v1 (RakNet bitstream, byte-aligned):
//   u8  version = kRadarRectVersion
//   f32 x       = left edge, pixels from left
//   f32 y       = top edge, pixels from top
//   f32 w       = width in pixels
//   f32 h       = height in pixels

namespace skr::protocol {

constexpr uint8_t kRpcRadarRect = 243;
constexpr uint8_t kRadarRectVersion = 1;

} // namespace skr::protocol
