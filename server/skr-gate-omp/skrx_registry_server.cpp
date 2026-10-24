#include "protocol/skrx_registry.h"
#include "protocol/skrx_gateway.h"
#include "protocol/client_state.h"
#include "protocol/session_info.h"
#include "protocol/radar_style.h"
#include "protocol/radar_rect.h"
#include "protocol/enhanced.h"
#include "protocol/rejection.h"
#include "protocol/native_api.h"

namespace skr::protocol::skrx {

bool RegisterBuiltinEntries(Registry& reg) {
    // ── Dedicated RPC inbound handlers ──
    // RPC 240, step 0 (raw, non-envelope): client state display data
    if (!reg.Register({kRpcClientState, 0, Direction::Inbound,
                       "client-state", 0, true, false, nullptr, nullptr}))
        return false;

    // RPC 243, step 0: radar rect from client
    if (!reg.Register({kRpcRadarRect, 0, Direction::Inbound,
                       "radar-rect", 0, true, false, nullptr, nullptr}))
        return false;

    // ── Dedicated RPC outbound entries ──
    // RPC 242: radar style/size to client
    if (!reg.Register({kRpcRadarStyle, 0, Direction::Outbound,
                       "radar-style", 0, false, false, nullptr, nullptr}))
        return false;

    // ── Enhanced envelope on RPC 240 (C->S) ──
    // Step 1: Enhanced Hello
    if (!reg.Register({kRpcClientState, static_cast<uint8_t>(EnhancedStep::Hello),
                       Direction::Inbound, "enhanced-hello", 0, false, false, nullptr, nullptr}))
        return false;

    // Step 3: Enhanced Confirm
    if (!reg.Register({kRpcClientState, static_cast<uint8_t>(EnhancedStep::Confirm),
                       Direction::Inbound, "enhanced-confirm", 0, false, false, nullptr, nullptr}))
        return false;

    // Step 5: API Hello
    if (!reg.Register({kRpcClientState, static_cast<uint8_t>(api::Step::Hello),
                       Direction::Inbound, "api-hello", 0, true, false, nullptr, nullptr}))
        return false;

    // Step 8: API Reply
    if (!reg.Register({kRpcClientState, static_cast<uint8_t>(api::Step::Reply),
                       Direction::Inbound, "api-reply", 0, true, true, nullptr, nullptr}))
        return false;

    // Step 9: Client Request (Phone-M4)
    if (!reg.Register({kRpcClientState, static_cast<uint8_t>(api::Step::ClientRequest),
                       Direction::Inbound, "client-request", 0, true, true, nullptr, nullptr}))
        return false;

    // ── Enhanced envelope on RPC 241 (S->C) ──
    // Step 0: raw session info
    if (!reg.Register({kRpcSessionInfo, 0, Direction::Outbound,
                       "session-info", 0, false, false, nullptr, nullptr}))
        return false;

    // Step 2: Enhanced Offer
    if (!reg.Register({kRpcSessionInfo, static_cast<uint8_t>(EnhancedStep::Offer),
                       Direction::Outbound, "enhanced-offer", 0, false, false, nullptr, nullptr}))
        return false;

    // Step 4: Rejection
    if (!reg.Register({kRpcSessionInfo, kEnhancedRejectionStep,
                       Direction::Outbound, "rejection", 0, false, false, nullptr, nullptr}))
        return false;

    // Step 6: API Welcome
    if (!reg.Register({kRpcSessionInfo, static_cast<uint8_t>(api::Step::Welcome),
                       Direction::Outbound, "api-welcome", 0, false, false, nullptr, nullptr}))
        return false;

    // Step 7: API Call
    if (!reg.Register({kRpcSessionInfo, static_cast<uint8_t>(api::Step::Call),
                       Direction::Outbound, "api-call", 0, false, false, nullptr, nullptr}))
        return false;

    // ── SKR-X Gateway (step 10) ──
    if (!reg.Register({kRpcClientState, kGatewayStep, Direction::Inbound,
                       "skrx-gateway-in", 0, true, true, nullptr, nullptr}))
        return false;

    if (!reg.Register({kRpcSessionInfo, kGatewayStep, Direction::Outbound,
                       "skrx-gateway-out", 0, false, false, nullptr, nullptr}))
        return false;

    return true;
}

} // namespace skr::protocol::skrx
