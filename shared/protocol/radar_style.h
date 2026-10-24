#pragma once
#include <cstdint>

// SKR radar-style RPC: SKR Gate (open.mp component) -> SKRClient.dll.
//
// Lets scripts pick the native radar's shape and size per player
// (natives SKR_SetRadarStyle / SKR_SetRadarSize, include skr_gate.inc).
// RPC ID 242 is SKR-private, chosen like 240/241 (session_info.h).
//
// Payload v1 (RakNet bitstream, byte-aligned):
//   u8  version  = 1
//   u8  style    = RadarStyle
//   u8  sizeMode = RadarSizeMode (Default or Custom)
//   f32 scale    = custom scale, [kRadarMinScale, kRadarMaxScale]; ignored for Default
//
// Payload v2 (backward-compatible extension):
//   u8  version  = 2
//   u8  style    = RadarStyle
//   u8  sizeMode = RadarSizeMode (Default, Custom, or Bounds)
//   f32 scale    = uniform scale (used for Custom; ignored for Default and Bounds)
//   u8  flags    = bit 0: visible (1=shown, 0=hidden)
//   f32 scaleW   = width scale  (only present when sizeMode == Bounds)
//   f32 scaleH   = height scale (only present when sizeMode == Bounds)
//
// The client applies it only on SKR sessions (after RPC 241); before that the
// radar is native (Round, Default).

namespace skr::protocol {

constexpr uint8_t kRpcRadarStyle = 242;
constexpr uint8_t kRadarStyleVersion = 2;

enum class RadarStyle : uint8_t {
    Round = 0,   // native GTA: circular mask + black ring
    Square = 1,  // no mask, no ring: tiles drawn as a rectangle
};

enum class RadarSizeMode : uint8_t {
    Default = 0,  // native GTA size
    Custom = 1,   // native size * scale (uniform)
    Bounds = 2,   // native size * scaleW/scaleH (independent width/height)
};

constexpr float kRadarMinScale = 0.5f;
constexpr float kRadarMaxScale = 2.0f;

// GTA V-style rectangular preset: wider than tall.  Applied when
// style == Square and sizeMode == Default (the "square" JSON default).
// Native GTA radar width/height are equal; these scale factors produce
// an aspect ratio close to GTA V's minimap.  One central definition.
constexpr float kSquarePresetScaleW = 1.30f;
constexpr float kSquarePresetScaleH = 0.90f;

// Flags byte (v2+)
constexpr uint8_t kRadarFlagVisible = 0x01;  // bit 0: radar is visible

} // namespace skr::protocol
