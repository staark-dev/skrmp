#pragma once
#include <cstdint>
#include "protocol/rejection.h"

namespace skr {

// GTA process exit codes interpreted by the launcher.
// Normal exit (0) ends the session; kExitRestartLobby causes the launcher
// to re-launch GTA with the lobby (like FiveM's "back to main menu").
constexpr uint32_t kExitRestartLobby = 42;
constexpr uint32_t kExitRejectionPrefix = 0x534B0000u; // "SK" + supported mask + reason

constexpr uint32_t ExitCodeForRejection(protocol::RejectionReason reason, uint8_t supportedClients) {
    return protocol::IsValidRejectionReason(reason)
        ? kExitRejectionPrefix |
              (static_cast<uint32_t>(supportedClients & protocol::kKnownSupportedClientsMask) << 8) |
              static_cast<uint8_t>(reason)
        : kExitRestartLobby;
}

constexpr protocol::RejectionReason RejectionForExitCode(uint32_t code) {
    if ((code & 0xFFFF0000u) != kExitRejectionPrefix) return protocol::RejectionReason::None;
    const auto reason = static_cast<protocol::RejectionReason>(code & 0xFFu);
    return protocol::IsValidRejectionReason(reason) ? reason : protocol::RejectionReason::None;
}

constexpr uint8_t SupportedClientsForExitCode(uint32_t code) {
    return RejectionForExitCode(code) == protocol::RejectionReason::None
        ? 0 : static_cast<uint8_t>((code >> 8) & 0xFFu);
}

} // namespace skr
