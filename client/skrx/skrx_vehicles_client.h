#pragma once
#include "protocol/skrx_vehicle.h"
#include <cstdint>

// SKR-X VEHICLES client-side: model registry, pool-slot assignment map,
// and CVehiclePool::Create hook for proxy→custom model swap.
//
// Threading: all calls on GTA main thread (RPC handlers + CGame::Process).

namespace skr::skrx::vehicles {

// Maximum concurrently tracked vehicle assignments.
constexpr int kMaxAssignments = 256;

struct ModelEntry {
    uint16_t customModelId = 0;
    uint16_t proxyModelId  = 0;
    uint16_t donorModelId  = 0;
    char     internalName[32] = {};
    bool     registered    = false;  // OP215 RegisterVehicle done
};

struct VehicleAssignment {
    uint16_t vehiclePoolId  = 0;
    uint16_t customModelId  = 0;
    uint32_t generation     = 0;
    bool     active         = false;
};

struct VehiclesClientState {
    ModelEntry models[protocol::skrx::vehicles::kMaxCustomModels];
    int modelCount = 0;

    VehicleAssignment assignments[kMaxAssignments];
    int assignCount = 0;

    bool hookInstalled = false;
    int swapCount      = 0;
    int deferCount     = 0;  // swaps skipped (model not loaded yet)
    int readySentCount = 0;
};

bool Init();
void Reset();

VehiclesClientState& GetState();

// Called from VEHICLES namespace dispatch in skrx_client.
void OnVehiclesMessage(const protocol::skrx::GatewayHeader& hdr,
                       const uint8_t* payload, size_t payloadLen);

// Install the CVehiclePool::Create hook (call after samp.dll is loaded).
bool InstallVehiclePoolHook(uintptr_t sampBase);

// Lookup: is this vehicle pool ID assigned to a custom model?
// Returns 0 if not found, or the custom model ID.
uint16_t GetAssignedModel(uint16_t vehiclePoolId);

// Lookup by custom model ID: is it in the registry?
const ModelEntry* FindModel(uint16_t customModelId);

} // namespace skr::skrx::vehicles
