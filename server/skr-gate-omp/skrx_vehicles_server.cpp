#include "skrx_vehicles_server.h"
#include "skr_gate_component.hpp"
#include "protocol/skrx_vehicle.h"
#include "protocol/skrx_gateway.h"

#include <cstring>
#include <cstdio>

// SKR-X VEHICLES M2.0 — Pawn natives (SKRX_* prefix).
// VehicleManager implementation is in skrx_vehicles_manager.cpp.

namespace {

using namespace skr::gate_omp;
using namespace skr::gate_omp::vehicles;
namespace proto = skr::protocol::skrx::vehicles;

static SkrGateComponent* s_gate = nullptr;
static VehicleManager*   s_mgr  = nullptr;

static IVehiclesComponent* GetVehicles() {
    return s_gate ? s_gate->GetVehiclesComponent() : nullptr;
}

// native bool:SKRX_IsCustomModel(modelid);
static cell AMX_NATIVE_CALL Native_IsCustomModel(AMX*, const cell* params) {
    if (!s_mgr) return 0;
    return s_mgr->IsCustomModel(static_cast<int>(params[1])) ? 1 : 0;
}

// native SKRX_CreateVehicle(modelid, Float:x, Float:y, Float:z, Float:rotation,
//                           color1, color2, respawnDelay);
static cell AMX_NATIVE_CALL Native_CreateVehicle(AMX*, const cell* params) {
    if (!s_gate || !s_mgr) return -1;

    const int modelId = static_cast<int>(params[1]);
    const float x = amx_ctof(params[2]);
    const float y = amx_ctof(params[3]);
    const float z = amx_ctof(params[4]);
    const float rotation = amx_ctof(params[5]);
    const int color1 = static_cast<int>(params[6]);
    const int color2 = static_cast<int>(params[7]);
    const int respawnDelay = static_cast<int>(params[8]);

    auto* vehicles = GetVehicles();
    if (!vehicles) return -1;

    if (proto::IsStockModelId(modelId)) {
        IVehicle* v = vehicles->create(false, modelId,
                                       Vector3(x, y, z), rotation,
                                       color1, color2,
                                       Seconds(respawnDelay), false);
        return v ? v->getID() : -1;
    }

    const auto* model = s_mgr->FindModel(static_cast<uint16_t>(modelId));
    if (!model) return -1;

    IVehicle* v = vehicles->create(false, model->proxyModelId,
                                   Vector3(x, y, z), rotation,
                                   color1, color2,
                                   Seconds(respawnDelay), false);
    if (!v) return -1;

    const uint32_t gen = s_mgr->TrackVehicle(v->getID(), model->customModelId);
    if (!gen) {
        vehicles->release(v->getID());
        return -1;
    }

    s_gate->BroadcastVehicleAssign(v->getID(), model->customModelId, gen);
    return v->getID();
}

// native bool:SKRX_DestroyVehicle(vehicleid);
static cell AMX_NATIVE_CALL Native_DestroyVehicle(AMX*, const cell* params) {
    if (!s_gate || !s_mgr) return 0;
    const int vehicleId = static_cast<int>(params[1]);
    if (!s_mgr->FindVehicle(vehicleId)) return 0;

    s_gate->BroadcastVehicleRevoke(static_cast<uint16_t>(vehicleId));
    s_mgr->UntrackVehicle(vehicleId);

    auto* vehicles = GetVehicles();
    if (vehicles) vehicles->release(vehicleId);
    return 1;
}

// native SKRX_GetVehicleModel(vehicleid);
static cell AMX_NATIVE_CALL Native_GetVehicleModel(AMX*, const cell* params) {
    const int vehicleId = static_cast<int>(params[1]);
    if (s_mgr) {
        uint16_t custom = s_mgr->GetCustomModel(vehicleId);
        if (custom) return static_cast<cell>(custom);
    }
    auto* vehicles = GetVehicles();
    if (!vehicles) return 0;
    auto* v = vehicles->get(vehicleId);
    return v ? v->getModel() : 0;
}

// native bool:SKRX_IsVehicleReadyForPlayer(playerid, vehicleid);
static cell AMX_NATIVE_CALL Native_IsVehicleReadyForPlayer(AMX*, const cell* params) {
    if (!s_mgr) return 0;
    return s_mgr->IsPlayerReady(static_cast<int>(params[1]),
                                static_cast<int>(params[2])) ? 1 : 0;
}

static const AMX_NATIVE_INFO kVehicleNatives[] = {
    {"SKRX_IsCustomModel",            &Native_IsCustomModel},
    {"SKRX_CreateVehicle",            &Native_CreateVehicle},
    {"SKRX_DestroyVehicle",           &Native_DestroyVehicle},
    {"SKRX_GetVehicleModel",          &Native_GetVehicleModel},
    {"SKRX_IsVehicleReadyForPlayer",  &Native_IsVehicleReadyForPlayer},
    {nullptr, nullptr}
};

} // anonymous namespace

namespace skr::gate_omp {

void RegisterVehicleNatives(IPawnScript& script, SkrGateComponent* gate,
                            vehicles::VehicleManager* mgr) {
    s_gate = gate;
    s_mgr = mgr;
    script.Register(kVehicleNatives, static_cast<int>(sizeof(kVehicleNatives) / sizeof(kVehicleNatives[0])));
}

void ReleaseVehicleNatives() {
    s_gate = nullptr;
    s_mgr = nullptr;
}

} // namespace skr::gate_omp
