#include "skrx_vehicles_server.h"

#include <cstring>

// SKR-X VEHICLES M2.0 — VehicleManager implementation.
// Standalone: no open.mp SDK dependency, no Pawn, no Gate component.
// Compiled both by the Gate DLL and by the unit test target.

namespace skr::gate_omp::vehicles {

// ── Model registry ──────────────────────────────────────────────────

bool VehicleManager::RegisterModel(uint16_t customModelId, uint16_t proxyModelId,
                                   uint16_t donorModelId, const char* internalName,
                                   const char* displayName, uint8_t seatCount) {
    if (!proto::IsCustomModelId(customModelId)) return false;
    if (!proto::IsStockModelId(proxyModelId)) return false;
    if (!proto::IsStockModelId(donorModelId)) return false;
    if (!internalName || !internalName[0]) return false;
    if (modelCount_ >= proto::kMaxCustomModels) return false;
    if (FindModel(customModelId)) return false;

    auto& m = models_[modelCount_++];
    m.customModelId = customModelId;
    m.proxyModelId = proxyModelId;
    m.donorModelId = donorModelId;
    m.seatCount = seatCount;
    m.enabled = true;

    strncpy(m.internalName, internalName, sizeof(m.internalName) - 1);
    m.internalName[sizeof(m.internalName) - 1] = '\0';

    if (displayName) {
        strncpy(m.displayName, displayName, sizeof(m.displayName) - 1);
        m.displayName[sizeof(m.displayName) - 1] = '\0';
    } else {
        strncpy(m.displayName, internalName, sizeof(m.displayName) - 1);
        m.displayName[sizeof(m.displayName) - 1] = '\0';
    }

    return true;
}

bool VehicleManager::UnregisterModel(uint16_t customModelId) {
    for (int i = 0; i < modelCount_; ++i) {
        if (models_[i].customModelId == customModelId) {
            models_[i] = models_[modelCount_ - 1];
            --modelCount_;
            return true;
        }
    }
    return false;
}

const RegisteredModel* VehicleManager::FindModel(uint16_t customModelId) const {
    for (int i = 0; i < modelCount_; ++i) {
        if (models_[i].customModelId == customModelId)
            return &models_[i];
    }
    return nullptr;
}

bool VehicleManager::IsCustomModel(int modelId) const {
    if (!proto::IsCustomModelId(modelId)) return false;
    return FindModel(static_cast<uint16_t>(modelId)) != nullptr;
}

int VehicleManager::FillRegistryEntries(proto::ModelRegistryEntry* out, int maxEntries) const {
    int count = 0;
    for (int i = 0; i < modelCount_ && count < maxEntries; ++i) {
        if (!models_[i].enabled) continue;
        auto& e = out[count];
        e.customModelId = models_[i].customModelId;
        e.proxyModelId = models_[i].proxyModelId;
        e.donorModelId = models_[i].donorModelId;
        e.nameLen = static_cast<uint8_t>(strlen(models_[i].internalName));
        if (e.nameLen > 31) e.nameLen = 31;
        memcpy(e.internalName, models_[i].internalName, e.nameLen);
        e.internalName[e.nameLen] = '\0';
        ++count;
    }
    return count;
}

// ── Vehicle lifecycle ───────────────────────────────────────────────

uint32_t VehicleManager::TrackVehicle(int vehiclePoolId, uint16_t customModelId) {
    if (vehiclePoolId < 0) return 0;
    if (!FindModel(customModelId)) return 0;
    if (vehicleCount_ >= proto::kMaxCustomVehicles) return 0;

    for (int i = 0; i < vehicleCount_; ++i) {
        if (vehicles_[i].vehiclePoolId == vehiclePoolId && vehicles_[i].alive)
            return 0;
    }

    auto& slot = vehicles_[vehicleCount_++];
    slot.vehiclePoolId = vehiclePoolId;
    slot.customModelId = customModelId;
    slot.generation = nextGeneration_++;
    slot.alive = true;
    return slot.generation;
}

bool VehicleManager::UntrackVehicle(int vehiclePoolId) {
    for (int i = 0; i < vehicleCount_; ++i) {
        if (vehicles_[i].vehiclePoolId == vehiclePoolId && vehicles_[i].alive) {
            vehicles_[i] = vehicles_[vehicleCount_ - 1];
            --vehicleCount_;
            return true;
        }
    }
    return false;
}

const CustomVehicleSlot* VehicleManager::FindVehicle(int vehiclePoolId) const {
    for (int i = 0; i < vehicleCount_; ++i) {
        if (vehicles_[i].vehiclePoolId == vehiclePoolId && vehicles_[i].alive)
            return &vehicles_[i];
    }
    return nullptr;
}

uint16_t VehicleManager::GetCustomModel(int vehiclePoolId) const {
    const auto* v = FindVehicle(vehiclePoolId);
    return v ? v->customModelId : 0;
}

uint32_t VehicleManager::RespawnVehicle(int vehiclePoolId) {
    for (int i = 0; i < vehicleCount_; ++i) {
        if (vehicles_[i].vehiclePoolId == vehiclePoolId && vehicles_[i].alive) {
            vehicles_[i].generation = nextGeneration_++;
            return vehicles_[i].generation;
        }
    }
    return 0;
}

int VehicleManager::FillActiveVehicles(proto::VehicleAssignData* out, int maxEntries) const {
    int count = 0;
    for (int i = 0; i < vehicleCount_ && count < maxEntries; ++i) {
        if (!vehicles_[i].alive) continue;
        auto& a = out[count];
        a.vehiclePoolId = static_cast<uint16_t>(vehicles_[i].vehiclePoolId);
        a.customModelId = vehicles_[i].customModelId;
        a.generation = vehicles_[i].generation;
        a.flags = 0;
        ++count;
    }
    return count;
}

// ── Per-player binding ──────────────────────────────────────────────

void VehicleManager::OnPlayerReady(int playerId, int vehiclePoolId,
                                   uint16_t customModelId, uint32_t generation,
                                   uint8_t status) {
    const auto* v = FindVehicle(vehiclePoolId);
    if (!v) return;
    if (v->customModelId != customModelId) return;
    if (v->generation != generation) return;

    auto& binding = playerBindings_[playerId][vehiclePoolId];
    binding.generation = generation;
    binding.ready = (status == 0);
}

bool VehicleManager::IsPlayerReady(int playerId, int vehiclePoolId) const {
    auto pit = playerBindings_.find(playerId);
    if (pit == playerBindings_.end()) return false;
    auto vit = pit->second.find(vehiclePoolId);
    if (vit == pit->second.end()) return false;

    const auto* v = FindVehicle(vehiclePoolId);
    if (!v) return false;

    return vit->second.ready && vit->second.generation == v->generation;
}

void VehicleManager::ForgetPlayer(int playerId) {
    playerBindings_.erase(playerId);
}

void VehicleManager::Reset() {
    modelCount_ = 0;
    vehicleCount_ = 0;
    nextGeneration_ = 1;
    playerBindings_.clear();
    memset(models_, 0, sizeof(models_));
    memset(vehicles_, 0, sizeof(vehicles_));
}

} // namespace skr::gate_omp::vehicles
