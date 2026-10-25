#include "skr_gate_component.hpp"
#include "protocol/skrx_gateway.h"
#include "protocol/skrx_registry.h"

// SKR-X Gateway server-side: step 10 inbound dispatch + CORE namespace handlers.
//
// Inner payload format (after gateway header decode):
//   u8 op  — namespace-specific operation
//   ...    — op-specific data

namespace skr::gate_omp {

using namespace skr::protocol;
using namespace skr::protocol::skrx;

void SkrGateComponent::OnGatewayMessage(IPlayer& player, const uint8_t* data, size_t len) {
    const int pid = player.getID();

    GatewayHeader hdr;
    const uint8_t* payload = nullptr;
    if (!DecodeGateway(data, len, hdr, payload)) {
        core_->logLn(LogLevel::Warning, "SKR-X: player %d: malformed gateway (%zu bytes)", pid, len);
        return;
    }

    if (!IsKnownNamespace(static_cast<uint8_t>(hdr.ns))) {
        core_->logLn(LogLevel::Warning, "SKR-X: player %d: unknown namespace %u",
                     pid, static_cast<unsigned>(hdr.ns));
        return;
    }

    switch (hdr.ns) {
    case Namespace::Core:
        OnGatewayCoreMessage(player, hdr, payload, hdr.payloadLen);
        break;
    case Namespace::Phone:
        OnGatewayPhoneMessage(player,hdr,payload,hdr.payloadLen);
        break;
    case Namespace::Vehicles:
        OnGatewayVehiclesMessage(player, hdr, payload, hdr.payloadLen);
        break;
    default:
        core_->logLn(LogLevel::Warning, "SKR-X: player %d: namespace %u not implemented",
                     pid, static_cast<unsigned>(hdr.ns));
        break;
    }
}

void SkrGateComponent::OnGatewayCoreMessage(IPlayer& player, const GatewayHeader& hdr,
                                             const uint8_t* payload, size_t payloadLen) {
    if (hdr.version != kCoreVersion) {
        core_->logLn(LogLevel::Warning, "SKR-X CORE: player %d: version %u unsupported",
                     player.getID(), hdr.version);
        return;
    }
    if (payloadLen < 1) return;

    const auto op = static_cast<CoreOp>(payload[0]);
    const uint8_t* opData = payload + 1;
    const size_t opLen = payloadLen - 1;

    switch (op) {
    case CoreOp::Ping:
        OnCorePing(player, opData, opLen);
        break;
    default:
        core_->logLn(LogLevel::Warning, "SKR-X CORE: player %d: unknown op %u",
                     player.getID(), static_cast<unsigned>(payload[0]));
        break;
    }
}

void SkrGateComponent::OnCorePing(IPlayer& player, const uint8_t* data, size_t len) {
    CorePing ping;
    if (!DecodePing(data, len, ping)) {
        core_->logLn(LogLevel::Warning, "SKR-X CORE:Ping: player %d: bad payload (%zu bytes)",
                     player.getID(), len);
        return;
    }

    const uint32_t serverMs = static_cast<uint32_t>(core_->getTickCount() / 1000);

    CorePong pong;
    pong.echoedTimestamp = ping.clientTimestamp;
    pong.serverTimestamp = serverMs;

    // Inner: u8 op + pong payload
    uint8_t inner[16];
    inner[0] = static_cast<uint8_t>(CoreOp::Pong);
    uint8_t pongBuf[8];
    const size_t pongLen = EncodePong(pong, pongBuf, sizeof(pongBuf));
    if (!pongLen) return;
    memcpy(inner + 1, pongBuf, pongLen);

    GatewayHeader outHdr;
    outHdr.ns = Namespace::Core;
    outHdr.version = kCoreVersion;
    outHdr.payloadLen = static_cast<uint16_t>(1 + pongLen);

    uint8_t buf[64];
    const size_t total = EncodeGateway(outHdr, inner, buf, sizeof(buf));
    if (!total) return;

    ApiSend(player, buf, total);

    core_->logLn(LogLevel::Message, "SKR-X CORE:Pong -> player %d (echo=%u server=%u)",
                 player.getID(), ping.clientTimestamp, serverMs);
}

// ── VEHICLES namespace handler ──────────────────────────────────────

void SkrGateComponent::OnGatewayVehiclesMessage(IPlayer& player, const GatewayHeader& hdr,
                                                 const uint8_t* payload, size_t payloadLen) {
    namespace veh = skr::protocol::skrx::vehicles;

    if (hdr.version != veh::kVehiclesVersion) {
        core_->logLn(LogLevel::Warning, "SKR-X VEHICLES: player %d: version %u unsupported",
                     player.getID(), hdr.version);
        return;
    }
    if (payloadLen < 1) return;

    const auto op = static_cast<veh::VehicleOp>(payload[0]);
    const uint8_t* opData = payload + 1;
    const size_t opLen = payloadLen - 1;

    switch (op) {
    case veh::VehicleOp::VehicleReady: {
        veh::VehicleReadyData ready;
        if (!veh::DecodeVehicleReady(opData, opLen, ready)) {
            core_->logLn(LogLevel::Warning, "SKR-X VEHICLES: player %d: bad VehicleReady", player.getID());
            return;
        }
        // A late ack for an instance this player no longer has streamed in
        // (stream-out already processed) must not mark the vehicle ready.
        IVehicle* vehicle = vehiclesComp_ ? vehiclesComp_->get(ready.vehiclePoolId) : nullptr;
        if (ready.status == 0 && (!vehicle || !vehicle->isStreamedInForPlayer(player))) {
            vehicleManager_.ClearPlayerVehicle(player.getID(), ready.vehiclePoolId);
            core_->logLn(LogLevel::Warning, "SKR-X VEHICLES: player %d: VehicleReady vehicle=%d gen=%u -> not-streamed-in",
                         player.getID(), ready.vehiclePoolId, ready.generation);
            break;
        }
        const auto result = vehicleManager_.OnPlayerReady(player.getID(), ready.vehiclePoolId,
                                                          ready.customModelId, ready.generation, ready.status);
        core_->logLn(result == vehicles::ReadyResult::Accepted || result == vehicles::ReadyResult::Duplicate
                         ? LogLevel::Message : LogLevel::Warning,
                     "SKR-X VEHICLES: player %d: VehicleReady vehicle=%d model=%d gen=%u status=%u -> %s",
                     player.getID(), ready.vehiclePoolId, ready.customModelId, ready.generation, ready.status,
                     vehicles::ReadyResultName(result));
        break;
    }
    default:
        core_->logLn(LogLevel::Warning, "SKR-X VEHICLES: player %d: unexpected op %u (client should not send this)",
                     player.getID(), static_cast<unsigned>(payload[0]));
        break;
    }
}

// ── Broadcast helpers (called from Pawn natives) ────────────────────

void SkrGateComponent::SendVehicleRegistryToPlayer(IPlayer& player) {
    namespace veh = skr::protocol::skrx::vehicles;

    veh::ModelRegistryEntry entries[veh::kMaxCustomModels];
    const int count = vehicleManager_.FillRegistryEntries(entries, veh::kMaxCustomModels);
    if (count <= 0) return;

    uint8_t inner[skrx::kMaxGatewayPayload];
    const size_t innerLen = veh::EncodeModelRegistry(entries, static_cast<uint8_t>(count), inner, sizeof(inner));
    if (!innerLen) return;

    uint8_t buf[skrx::kMaxGatewaySize];
    const size_t total = veh::EncodeVehiclesGateway(veh::VehicleOp::ModelRegistry, inner, innerLen, buf, sizeof(buf));
    if (!total) return;

    ApiSend(player, buf, total);
}

void SkrGateComponent::BroadcastVehicleAssign(int vehiclePoolId, uint16_t customModelId, uint32_t generation) {
    namespace veh = skr::protocol::skrx::vehicles;

    veh::VehicleAssignData assign;
    assign.vehiclePoolId = static_cast<uint16_t>(vehiclePoolId);
    assign.customModelId = customModelId;
    assign.generation = generation;
    assign.flags = 0;

    uint8_t inner[veh::kVehicleAssignSize];
    const size_t innerLen = veh::EncodeVehicleAssign(assign, inner, sizeof(inner));
    if (!innerLen) return;

    uint8_t buf[skrx::kMaxGatewaySize];
    const size_t total = veh::EncodeVehiclesGateway(veh::VehicleOp::VehicleAssign, inner, innerLen, buf, sizeof(buf));
    if (!total) return;

    // Send to all enhanced + gateway-ready players.
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [pid, data] : players_) {
        if (data.enhanced.mode != protocol::ClientMode::Enhanced) continue;
        if (!data.api.ready) continue;
        if (!(data.api.capabilities & protocol::api::CapGateway)) continue;
        IPlayer* p = core_->getPlayers().get(pid);
        if (p) ApiSend(*p, buf, total);
    }
}

void SkrGateComponent::BroadcastVehicleRevoke(uint16_t vehiclePoolId) {
    namespace veh = skr::protocol::skrx::vehicles;

    uint8_t inner[veh::kVehicleRevokeSize];
    const size_t innerLen = veh::EncodeVehicleRevoke(vehiclePoolId, inner, sizeof(inner));
    if (!innerLen) return;

    uint8_t buf[skrx::kMaxGatewaySize];
    const size_t total = veh::EncodeVehiclesGateway(veh::VehicleOp::VehicleRevoke, inner, innerLen, buf, sizeof(buf));
    if (!total) return;

    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [pid, data] : players_) {
        if (data.enhanced.mode != protocol::ClientMode::Enhanced) continue;
        if (!data.api.ready) continue;
        if (!(data.api.capabilities & protocol::api::CapGateway)) continue;
        IPlayer* p = core_->getPlayers().get(pid);
        if (p) ApiSend(*p, buf, total);
    }
}

// ── Per-player batch assign (sent on API ready) ────────────────────

void SkrGateComponent::SendVehicleBatchAssignToPlayer(IPlayer& player) {
    namespace veh = skr::protocol::skrx::vehicles;

    veh::VehicleAssignData entries[veh::kMaxBatchEntries];
    const int count = vehicleManager_.FillActiveVehicles(entries, veh::kMaxBatchEntries);
    if (count <= 0) return;

    uint8_t inner[skrx::kMaxGatewayPayload];
    const size_t innerLen = veh::EncodeBatchAssign(entries, static_cast<uint8_t>(count), inner, sizeof(inner));
    if (!innerLen) return;

    uint8_t buf[skrx::kMaxGatewaySize];
    const size_t total = veh::EncodeVehiclesGateway(veh::VehicleOp::VehicleBatchAssign, inner, innerLen, buf, sizeof(buf));
    if (!total) return;

    ApiSend(player, buf, total);
    core_->logLn(LogLevel::Message, "SKR-X VEHICLES: batch assign %d vehicles -> player %d",
                 count, player.getID());
}

// ── VehicleEventHandler — per-player stream-in ─────────────────────

void SkrGateComponent::onVehicleStreamIn(IVehicle& vehicle, IPlayer& player) {
    const int vehicleId = vehicle.getID();
    const uint16_t customModel = vehicleManager_.GetCustomModel(vehicleId);
    if (!customModel) return;

    const auto* slot = vehicleManager_.FindVehicle(vehicleId);
    if (!slot) return;

    const int pid = player.getID();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = players_.find(pid);
        if (it == players_.end()) return;
        if (it->second.enhanced.mode != protocol::ClientMode::Enhanced) return;
        if (!it->second.api.ready) return;
        if (!(it->second.api.capabilities & protocol::api::CapGateway)) return;
    }

    namespace veh = skr::protocol::skrx::vehicles;

    veh::VehicleAssignData assign;
    assign.vehiclePoolId = static_cast<uint16_t>(vehicleId);
    assign.customModelId = customModel;
    assign.generation = slot->generation;
    assign.flags = 0;

    uint8_t inner[veh::kVehicleAssignSize];
    const size_t innerLen = veh::EncodeVehicleAssign(assign, inner, sizeof(inner));
    if (!innerLen) return;

    uint8_t buf[skrx::kMaxGatewaySize];
    const size_t total = veh::EncodeVehiclesGateway(veh::VehicleOp::VehicleAssign, inner, innerLen, buf, sizeof(buf));
    if (!total) return;

    ApiSend(player, buf, total);
    core_->logLn(LogLevel::Message, "skrx vehicles: bind player=%d vehicleid=%d model=%d generation=%u",
                 pid, vehicleId, customModel, slot->generation);
}

// The client destroys its instance on stream-out; readiness must be
// re-acknowledged by the next instance it creates.
void SkrGateComponent::onVehicleStreamOut(IVehicle& vehicle, IPlayer& player) {
    vehicleManager_.ClearPlayerVehicle(player.getID(), vehicle.getID());
}

} // namespace skr::gate_omp
