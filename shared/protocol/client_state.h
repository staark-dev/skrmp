#pragma once
#include <cstdint>

// SKR client-state RPC: SKRClient.dll -> SKR Gate (open.mp component).
//
// RPC ID 240 is SKR-private. open.mp's NetCode uses RPC IDs up to 185 and LegacyNetwork
// registers hooks for every ID 0..253, so 240 reaches per-RPC handlers without colliding
// with SA-MP/open.mp traffic (checked against openmultiplayer/open.mp Shared/NetCode).
//
// Payload v1 (RakNet bitstream, byte-aligned, little-endian, written in this order):
//   u8    version            = kClientStateVersion
//   f32   x, y, z            player position (world units)
//   f32   heading            degrees, GTA convention (counter-clockwise from north), 0..360
//   f32   radarRange         vanilla CRadar range (world units)
//   f32   radarOrient        vanilla radar rotation, degrees -180..180
//   u16   speedKmh           vehicle speed, 0 on foot
//   i8    gear               -1 on foot, 0 reverse, 1.. forward
//   u16   maxKmh             handling max velocity in km/h, 0 = unknown
//   u8    cityLen, cityLen x char   GTA zone name, <= kClientStateMaxName
//   u8    areaLen, areaLen x char
//
// Display-only data: the server never trusts it for authority decisions.

namespace skr::protocol {

constexpr uint8_t kRpcClientState = 240;
constexpr uint8_t kClientStateVersion = 1;
constexpr uint8_t kClientStateMaxName = 47;

} // namespace skr::protocol
