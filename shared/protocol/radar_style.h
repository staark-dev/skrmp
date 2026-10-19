#pragma once
#include <cstdint>

// SKR radar-style RPC: SKR Gate (open.mp component) -> SKRClient.dll.
//
// Lets scripts pick the native radar's shape and size per player
// (natives SKR_SetRadarStyle / SKR_SetRadarSize, include skr_gate.inc).
// RPC ID 242 is SKR-private, chosen like 240/241 (session_info.h).
//
// Payload v1 (RakNet bitstream, byte-aligned):
//   u8  version  = kRadarStyleVersion
//   u8  style    = RadarStyle
//   u8  sizeMode = RadarSizeMode
//   f32 scale    = custom scale, [kRadarMinScale, kRadarMaxScale]; ignored for Default
//
// The client applies it only on SKR sessions (after RPC 241); before that the
// radar is native (Round, Default).

namespace skr::protocol {

constexpr uint8_t kRpcRadarStyle = 242;
constexpr uint8_t kRadarStyleVersion = 1;

enum class RadarStyle : uint8_t {
    Round = 0,   // native GTA: circular mask + black ring
    Square = 1,  // no mask, no ring: tiles drawn as a rectangle
};

enum class RadarSizeMode : uint8_t {
    Default = 0,  // native GTA size
    Custom = 1,   // native size * scale
};

constexpr float kRadarMinScale = 0.5f;
constexpr float kRadarMaxScale = 2.0f;

} // namespace skr::protocol
