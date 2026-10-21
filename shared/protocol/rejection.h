#pragma once

#include "protocol/enhanced.h"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace skr::protocol {

// Backward-compatible extension of the existing Enhanced RPC 241 envelope.
// Older SKR clients ignore this step because it is not EnhancedStep::Offer.
// Vanilla clients never parse this payload and receive only the standard
// SA-MP RPC 130 rejection result.
constexpr uint8_t kEnhancedRejectionStep = 4;
constexpr uint8_t kRejectionVersion = 1;

enum class RejectionReason : uint8_t {
    None = 0,
    UnsupportedClientVersion = 1,
    ClientVersionNotAllowed = 2,
    EnhancedRequired = 3,
    AuthenticationFailed = 4,
    EnhancedNegotiationTimeout = 5
};

enum SupportedClient : uint8_t {
    SupportedSamp037 = 1u << 0,
    SupportedSkrEnhanced = 1u << 1,
    SupportedOpenMp = 1u << 2,
    SupportedSamp03DL = 1u << 3
};
constexpr uint8_t kKnownSupportedClientsMask = SupportedSamp037 | SupportedSkrEnhanced |
    SupportedOpenMp | SupportedSamp03DL;

#pragma pack(push, 1)
struct RejectionMessage {
    uint8_t marker = kEnhancedMarker;
    uint8_t step = kEnhancedRejectionStep;
    uint8_t version = kRejectionVersion;
    uint8_t reason = 0;
    uint8_t supportedClients = 0;
    uint8_t reserved[3]{};
};
#pragma pack(pop)

static_assert(sizeof(RejectionMessage) == 8, "RPC 241 rejection envelope must remain 8 bytes");

constexpr bool IsValidRejectionReason(RejectionReason reason) {
    return reason >= RejectionReason::UnsupportedClientVersion &&
           reason <= RejectionReason::EnhancedNegotiationTimeout;
}

inline bool DecodeRejection(const uint8_t* data, size_t size, RejectionMessage& out) {
    if (!data || size != sizeof(RejectionMessage)) return false;
    std::memcpy(&out, data, sizeof(out));
    if (out.marker != kEnhancedMarker || out.step != kEnhancedRejectionStep ||
        out.version != kRejectionVersion ||
        !IsValidRejectionReason(static_cast<RejectionReason>(out.reason)) ||
        (out.supportedClients & ~kKnownSupportedClientsMask) != 0) return false;
    return out.reserved[0] == 0 && out.reserved[1] == 0 && out.reserved[2] == 0;
}

constexpr const char* RejectionCode(RejectionReason reason) {
    switch (reason) {
        case RejectionReason::UnsupportedClientVersion: return "UNSUPPORTED_CLIENT_VERSION";
        case RejectionReason::ClientVersionNotAllowed: return "CLIENT_VERSION_NOT_ALLOWED";
        case RejectionReason::EnhancedRequired: return "ENHANCED_REQUIRED";
        case RejectionReason::AuthenticationFailed: return "AUTHENTICATION_FAILED";
        case RejectionReason::EnhancedNegotiationTimeout: return "ENHANCED_NEGOTIATION_TIMEOUT";
        default: return "UNKNOWN_REJECTION";
    }
}

inline std::string SupportedClientsText(uint8_t mask) {
    std::string value;
    const auto append = [&value](const char* name) {
        if (!value.empty()) value += ", ";
        value += name;
    };
    if (mask & SupportedSamp037) append("SA-MP 0.3.7");
    if (mask & SupportedSkrEnhanced) append("SKR Client");
    if (mask & SupportedOpenMp) append("open.mp");
    if (mask & SupportedSamp03DL) append("SA-MP 0.3.DL");
    return value.empty() ? "none" : value;
}

inline std::string RejectionDisplayText(RejectionReason reason, uint8_t supportedClients) {
    std::string value = "[";
    value += RejectionCode(reason);
    value += "] ";
    switch (reason) {
        case RejectionReason::UnsupportedClientVersion:
            value += "This client protocol is not supported.";
            break;
        case RejectionReason::ClientVersionNotAllowed:
            value += "This client version is not allowed by the server.";
            break;
        case RejectionReason::EnhancedRequired:
            value += "SKR Enhanced is required for this server.";
            break;
        case RejectionReason::AuthenticationFailed:
            value += "Authentication failed. Start the game from SKR Launcher and try again.";
            break;
        case RejectionReason::EnhancedNegotiationTimeout:
            value += "SKR Enhanced negotiation timed out. Update or restart SKR Client.";
            break;
        default:
            value += "The server rejected the connection.";
            break;
    }
    value += " Supported clients: ";
    value += SupportedClientsText(supportedClients);
    value += ".";
    return value;
}

} // namespace skr::protocol
