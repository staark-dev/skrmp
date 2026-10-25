#pragma once

#include "protocol/skrx_vehicle.h"
#include <cstdint>
#include <cstring>
#include <unordered_map>

// SKR-X VEHICLES M2 — server-side model registry and vehicle lifecycle.
//
// The registry holds the set of custom models this server supports.
// Each spawned custom vehicle is tracked as a CustomVehicleSlot, mapping
// an open.mp vehicle pool ID to a custom model + generation.
//
// Thread safety: all public methods are called from the main thread only
// (Pawn natives run on main, onTick is main, event handlers are main).

namespace skr::gate_omp::vehicles {

namespace proto = skr::protocol::skrx::vehicles;

// ── Model registry (server config) ─────────────────────────────────

struct RegisteredModel {
    uint16_t customModelId;
    uint16_t proxyModelId;
    uint16_t donorModelId;
    char     internalName[32];
    char     displayName[64];
    uint8_t  seatCount;
    bool     enabled;
};

// ── Vehicle instance (one per spawned custom vehicle) ───────────────

struct CustomVehicleSlot {
    int      vehiclePoolId;       // open.mp pool slot
    uint16_t customModelId;
    uint32_t generation;          // increments on respawn
    bool     alive;
};

// ── Per-player per-vehicle binding ──────────────────────────────────

struct PlayerBinding {
    uint32_t generation;
    bool     ready;               // player confirmed model loaded
};

// ── VehicleReady validation result ──────────────────────────────────

enum class ReadyResult : uint8_t {
    Accepted = 0,     // binding set ready for the current generation
    Duplicate,        // already ready for this generation (idempotent)
    NotReady,         // status != 0: binding cleared for this generation
    InvalidPlayer,
    InvalidStatus,
    InvalidModel,     // not a registered custom model
    UnknownVehicle,   // pool ID not tracked as custom
    ModelMismatch,
    StaleGeneration,
};

const char* ReadyResultName(ReadyResult result);

// ── Vehicle Manager ─────────────────────────────────────────────────

class VehicleManager {
public:
    // ── Model registry ──────────────────────────────────────────────

    bool RegisterModel(uint16_t customModelId, uint16_t proxyModelId,
                       uint16_t donorModelId, const char* internalName,
                       const char* displayName, uint8_t seatCount);

    bool UnregisterModel(uint16_t customModelId);

    const RegisteredModel* FindModel(uint16_t customModelId) const;

    bool IsCustomModel(int modelId) const;

    int ModelCount() const { return modelCount_; }

    const RegisteredModel* Models() const { return models_; }

    // Fill protocol registry entries for sending to clients.
    int FillRegistryEntries(proto::ModelRegistryEntry* out, int maxEntries) const;

    // ── Vehicle lifecycle ───────────────────────────────────────────

    // Create: associates an open.mp vehicle pool ID with a custom model.
    // Returns the generation token (>0) on success, 0 on failure.
    // The caller must create the proxy vehicle in open.mp first.
    uint32_t TrackVehicle(int vehiclePoolId, uint16_t customModelId);

    // Destroy: removes tracking for a vehicle pool ID.
    // Returns true if the vehicle was tracked.
    bool UntrackVehicle(int vehiclePoolId);

    // Lookup by pool ID.
    const CustomVehicleSlot* FindVehicle(int vehiclePoolId) const;

    // Get the custom model for a pool ID (0 if not custom).
    uint16_t GetCustomModel(int vehiclePoolId) const;

    // Respawn: increments generation. Returns new generation (0 on failure).
    uint32_t RespawnVehicle(int vehiclePoolId);

    int VehicleCount() const { return vehicleCount_; }

    // Fill all tracked vehicles for batch-assign to a joining player.
    int FillActiveVehicles(proto::VehicleAssignData* out, int maxEntries) const;

    // ── Per-player binding ──────────────────────────────────────────

    // Readiness is per player and per vehicle instance: it must match the
    // tracked pool ID, custom model and current generation.
    ReadyResult OnPlayerReady(int playerId, int vehiclePoolId, uint16_t customModelId,
                              uint32_t generation, uint8_t status);

    bool IsPlayerReady(int playerId, int vehiclePoolId) const;

    // Vehicle streamed out for this player: the client destroyed its instance.
    void ClearPlayerVehicle(int playerId, int vehiclePoolId);

    void ForgetPlayer(int playerId);

    // ── Reset ───────────────────────────────────────────────────────

    void Reset();

private:
    RegisteredModel models_[proto::kMaxCustomModels] = {};
    int modelCount_ = 0;

    CustomVehicleSlot vehicles_[proto::kMaxCustomVehicles] = {};
    int vehicleCount_ = 0;
    uint32_t nextGeneration_ = 1;

    // playerId -> { vehiclePoolId -> binding }
    std::unordered_map<int, std::unordered_map<int, PlayerBinding>> playerBindings_;
};

} // namespace skr::gate_omp::vehicles
