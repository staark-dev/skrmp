#pragma once
#include <cstdint>
#include <cstring>

#include "protocol/enhanced.h"
#include "protocol/skrx_gateway.h"

// SKR-X VEHICLES namespace (M2) — wire protocol.
//
// Namespace 1 on the Gateway (step 10). Version 1.
// All multi-byte fields are little-endian. Messages are length-exact.
//
// The server creates stock proxy vehicles through open.mp's vehicle pool.
// This protocol tells enhanced clients which pool-slot vehicles are actually
// custom models, so they can swap the visual model client-side.

namespace skr::protocol::skrx::vehicles {

constexpr uint8_t kVehiclesVersion = 1;

// Maximum custom models a server can register.
constexpr int kMaxCustomModels = 32;

// Maximum vehicles tracked at once (mirrors SA-MP pool limit).
constexpr int kMaxCustomVehicles = 256;

// Maximum entries in a batch assign message.
constexpr int kMaxBatchEntries = 20;

// ── Operations ──────────────────────────────────────────────────────

enum class VehicleOp : uint8_t {
    // S → C: model registry (sent once after gateway ready)
    ModelRegistry     = 1,
    // S → C: assign custom model to a vehicle pool slot
    VehicleAssign     = 2,
    // S → C: revoke custom model from a vehicle pool slot
    VehicleRevoke     = 3,
    // C → S: client confirms model loaded for a vehicle
    VehicleReady      = 4,
    // S → C: batch assign (multiple vehicles at once)
    VehicleBatchAssign = 5,
    // S → C: generation update (respawn)
    VehicleRespawn    = 6,
};

// ── Model registry entry (shared between server config and wire) ────

struct ModelRegistryEntry {
    uint16_t customModelId  = 0;  // 612, 613, 614, ...
    uint16_t proxyModelId   = 0;  // stock model in open.mp pool (400–611)
    uint16_t donorModelId   = 0;  // for client CVehicleModelInfo cloning
    uint8_t  nameLen        = 0;
    char     internalName[32] = {};
};

// ── Vehicle assignment ──────────────────────────────────────────────

struct VehicleAssignData {
    uint16_t vehiclePoolId  = 0;
    uint16_t customModelId  = 0;
    uint32_t generation     = 0;
    uint8_t  flags          = 0;  // reserved
};

// ── Vehicle ready (client → server) ─────────────────────────────────

struct VehicleReadyData {
    uint16_t vehiclePoolId  = 0;
    uint16_t customModelId  = 0;
    uint32_t generation     = 0;
    uint8_t  status         = 0;  // 0 = loaded, 1 = asset missing, 2 = failed
};

// ── Codecs ──────────────────────────────────────────────────────────

// ModelRegistry (Op 1, S→C)
// Wire: u8 count + array of { u16 customModelId, u16 proxyModelId, u16 donorModelId, u8 nameLen, char[nameLen] }
inline size_t EncodeModelRegistry(const ModelRegistryEntry* entries, uint8_t count,
                                  uint8_t* out, size_t cap) {
    if (count > kMaxCustomModels) return 0;
    size_t pos = 0;
    if (pos + 1 > cap) return 0;
    out[pos++] = count;
    for (uint8_t i = 0; i < count; ++i) {
        const auto& e = entries[i];
        if (e.nameLen > 31) return 0;
        if (pos + 7 + e.nameLen > cap) return 0;
        out[pos++] = uint8_t(e.customModelId);
        out[pos++] = uint8_t(e.customModelId >> 8);
        out[pos++] = uint8_t(e.proxyModelId);
        out[pos++] = uint8_t(e.proxyModelId >> 8);
        out[pos++] = uint8_t(e.donorModelId);
        out[pos++] = uint8_t(e.donorModelId >> 8);
        out[pos++] = e.nameLen;
        memcpy(out + pos, e.internalName, e.nameLen);
        pos += e.nameLen;
    }
    return pos;
}

inline bool DecodeModelRegistry(const uint8_t* data, size_t len,
                                ModelRegistryEntry* entries, uint8_t& count) {
    if (!data || len < 1) return false;
    count = data[0];
    if (count > kMaxCustomModels) return false;
    size_t pos = 1;
    for (uint8_t i = 0; i < count; ++i) {
        if (pos + 7 > len) return false;
        entries[i].customModelId = uint16_t(data[pos]) | (uint16_t(data[pos+1]) << 8); pos += 2;
        entries[i].proxyModelId  = uint16_t(data[pos]) | (uint16_t(data[pos+1]) << 8); pos += 2;
        entries[i].donorModelId  = uint16_t(data[pos]) | (uint16_t(data[pos+1]) << 8); pos += 2;
        entries[i].nameLen = data[pos++];
        if (entries[i].nameLen > 31 || pos + entries[i].nameLen > len) return false;
        memcpy(entries[i].internalName, data + pos, entries[i].nameLen);
        entries[i].internalName[entries[i].nameLen] = '\0';
        pos += entries[i].nameLen;
    }
    return pos == len;
}

// VehicleAssign (Op 2, S→C)
// Wire: u16 vehiclePoolId + u16 customModelId + u32 generation + u8 flags = 9 bytes
constexpr size_t kVehicleAssignSize = 9;

inline size_t EncodeVehicleAssign(const VehicleAssignData& a, uint8_t* out, size_t cap) {
    if (cap < kVehicleAssignSize) return 0;
    out[0] = uint8_t(a.vehiclePoolId);
    out[1] = uint8_t(a.vehiclePoolId >> 8);
    out[2] = uint8_t(a.customModelId);
    out[3] = uint8_t(a.customModelId >> 8);
    out[4] = uint8_t(a.generation);
    out[5] = uint8_t(a.generation >> 8);
    out[6] = uint8_t(a.generation >> 16);
    out[7] = uint8_t(a.generation >> 24);
    out[8] = a.flags;
    return kVehicleAssignSize;
}

inline bool DecodeVehicleAssign(const uint8_t* data, size_t len, VehicleAssignData& a) {
    if (len != kVehicleAssignSize) return false;
    a.vehiclePoolId = uint16_t(data[0]) | (uint16_t(data[1]) << 8);
    a.customModelId = uint16_t(data[2]) | (uint16_t(data[3]) << 8);
    a.generation    = uint32_t(data[4]) | (uint32_t(data[5]) << 8) |
                      (uint32_t(data[6]) << 16) | (uint32_t(data[7]) << 24);
    a.flags = data[8];
    return true;
}

// VehicleRevoke (Op 3, S→C)
// Wire: u16 vehiclePoolId = 2 bytes
constexpr size_t kVehicleRevokeSize = 2;

inline size_t EncodeVehicleRevoke(uint16_t vehiclePoolId, uint8_t* out, size_t cap) {
    if (cap < kVehicleRevokeSize) return 0;
    out[0] = uint8_t(vehiclePoolId);
    out[1] = uint8_t(vehiclePoolId >> 8);
    return kVehicleRevokeSize;
}

inline bool DecodeVehicleRevoke(const uint8_t* data, size_t len, uint16_t& vehiclePoolId) {
    if (len != kVehicleRevokeSize) return false;
    vehiclePoolId = uint16_t(data[0]) | (uint16_t(data[1]) << 8);
    return true;
}

// VehicleReady (Op 4, C→S)
// Wire: u16 vehiclePoolId + u16 customModelId + u32 generation + u8 status = 9 bytes
constexpr size_t kVehicleReadySize = 9;

inline size_t EncodeVehicleReady(const VehicleReadyData& r, uint8_t* out, size_t cap) {
    if (cap < kVehicleReadySize) return 0;
    out[0] = uint8_t(r.vehiclePoolId);
    out[1] = uint8_t(r.vehiclePoolId >> 8);
    out[2] = uint8_t(r.customModelId);
    out[3] = uint8_t(r.customModelId >> 8);
    out[4] = uint8_t(r.generation);
    out[5] = uint8_t(r.generation >> 8);
    out[6] = uint8_t(r.generation >> 16);
    out[7] = uint8_t(r.generation >> 24);
    out[8] = r.status;
    return kVehicleReadySize;
}

inline bool DecodeVehicleReady(const uint8_t* data, size_t len, VehicleReadyData& r) {
    if (len != kVehicleReadySize) return false;
    r.vehiclePoolId = uint16_t(data[0]) | (uint16_t(data[1]) << 8);
    r.customModelId = uint16_t(data[2]) | (uint16_t(data[3]) << 8);
    r.generation    = uint32_t(data[4]) | (uint32_t(data[5]) << 8) |
                      (uint32_t(data[6]) << 16) | (uint32_t(data[7]) << 24);
    r.status = data[8];
    return true;
}

// VehicleBatchAssign (Op 5, S→C)
// Wire: u8 count + array of { u16 vehiclePoolId, u16 customModelId, u32 generation }
inline size_t EncodeBatchAssign(const VehicleAssignData* entries, uint8_t count,
                                uint8_t* out, size_t cap) {
    if (count > kMaxBatchEntries) return 0;
    const size_t needed = 1 + count * 8;
    if (cap < needed) return 0;
    out[0] = count;
    size_t pos = 1;
    for (uint8_t i = 0; i < count; ++i) {
        const auto& a = entries[i];
        out[pos++] = uint8_t(a.vehiclePoolId);
        out[pos++] = uint8_t(a.vehiclePoolId >> 8);
        out[pos++] = uint8_t(a.customModelId);
        out[pos++] = uint8_t(a.customModelId >> 8);
        out[pos++] = uint8_t(a.generation);
        out[pos++] = uint8_t(a.generation >> 8);
        out[pos++] = uint8_t(a.generation >> 16);
        out[pos++] = uint8_t(a.generation >> 24);
    }
    return pos;
}

inline bool DecodeBatchAssign(const uint8_t* data, size_t len,
                              VehicleAssignData* entries, uint8_t& count) {
    if (!data || len < 1) return false;
    count = data[0];
    if (count > kMaxBatchEntries) return false;
    if (len != 1 + count * 8) return false;
    size_t pos = 1;
    for (uint8_t i = 0; i < count; ++i) {
        entries[i].vehiclePoolId = uint16_t(data[pos]) | (uint16_t(data[pos+1]) << 8); pos += 2;
        entries[i].customModelId = uint16_t(data[pos]) | (uint16_t(data[pos+1]) << 8); pos += 2;
        entries[i].generation    = uint32_t(data[pos]) | (uint32_t(data[pos+1]) << 8) |
                                   (uint32_t(data[pos+2]) << 16) | (uint32_t(data[pos+3]) << 24); pos += 4;
        entries[i].flags = 0;
    }
    return true;
}

// VehicleRespawn (Op 6, S→C)
// Wire: u16 vehiclePoolId + u32 newGeneration = 6 bytes
constexpr size_t kVehicleRespawnSize = 6;

inline size_t EncodeVehicleRespawn(uint16_t vehiclePoolId, uint32_t generation,
                                   uint8_t* out, size_t cap) {
    if (cap < kVehicleRespawnSize) return 0;
    out[0] = uint8_t(vehiclePoolId);
    out[1] = uint8_t(vehiclePoolId >> 8);
    out[2] = uint8_t(generation);
    out[3] = uint8_t(generation >> 8);
    out[4] = uint8_t(generation >> 16);
    out[5] = uint8_t(generation >> 24);
    return kVehicleRespawnSize;
}

inline bool DecodeVehicleRespawn(const uint8_t* data, size_t len,
                                 uint16_t& vehiclePoolId, uint32_t& generation) {
    if (len != kVehicleRespawnSize) return false;
    vehiclePoolId = uint16_t(data[0]) | (uint16_t(data[1]) << 8);
    generation    = uint32_t(data[2]) | (uint32_t(data[3]) << 8) |
                    (uint32_t(data[4]) << 16) | (uint32_t(data[5]) << 24);
    return true;
}

// ── Full gateway message helpers ────────────────────────────────────

inline size_t EncodeVehiclesGateway(VehicleOp op, const uint8_t* inner, size_t innerLen,
                                    uint8_t* out, size_t cap) {
    const size_t payloadLen = 1 + innerLen;
    if (payloadLen > kMaxGatewayPayload) return 0;
    uint8_t payload[kMaxGatewayPayload + 1];
    payload[0] = static_cast<uint8_t>(op);
    if (innerLen) memcpy(payload + 1, inner, innerLen);
    GatewayHeader hdr;
    hdr.ns = Namespace::Vehicles;
    hdr.version = kVehiclesVersion;
    hdr.payloadLen = static_cast<uint16_t>(payloadLen);
    return EncodeGateway(hdr, payload, out, cap);
}

// Validation: is this a valid custom model ID? (612+, not in stock range)
inline bool IsCustomModelId(int modelId) {
    return modelId >= 612 && modelId <= 32767;
}

// Validation: is this a valid stock/proxy model ID?
inline bool IsStockModelId(int modelId) {
    return modelId >= 400 && modelId <= 611;
}

} // namespace skr::protocol::skrx::vehicles
