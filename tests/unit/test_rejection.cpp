#include "protocol/rejection.h"
#include "exit_codes.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

namespace {
int checks = 0;

void Check(bool condition, const char* message) {
    ++checks;
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}
} // namespace

int main() {
    using namespace skr::protocol;
    const uint8_t supported = SupportedSamp037 | SupportedSkrEnhanced | SupportedOpenMp;
    const RejectionReason reasons[] = {
        RejectionReason::UnsupportedClientVersion,
        RejectionReason::ClientVersionNotAllowed,
        RejectionReason::EnhancedRequired,
        RejectionReason::AuthenticationFailed,
        RejectionReason::EnhancedNegotiationTimeout
    };

    for (RejectionReason reason : reasons) {
        RejectionMessage wire{};
        wire.reason = static_cast<uint8_t>(reason);
        wire.supportedClients = supported;
        RejectionMessage decoded{};
        Check(DecodeRejection(reinterpret_cast<const uint8_t*>(&wire), sizeof(wire), decoded),
              "valid rejection envelope decodes");
        Check(static_cast<RejectionReason>(decoded.reason) == reason, "reason round-trips");
        Check(std::string(RejectionCode(reason)).find("UNKNOWN") == std::string::npos,
              "stable error code exists");
        const std::string display = RejectionDisplayText(reason, supported);
        Check(display.find(RejectionCode(reason)) != std::string::npos, "display contains stable code");
        Check(display.find("SA-MP 0.3.7, SKR Client, open.mp") != std::string::npos,
              "display contains enforced supported-client list");
        const uint32_t exitCode = skr::ExitCodeForRejection(reason, supported);
        Check(skr::RejectionForExitCode(exitCode) == reason, "launcher exit code preserves reason");
        Check(skr::SupportedClientsForExitCode(exitCode) == supported,
              "launcher exit code preserves supported-client mask");
    }

    RejectionMessage invalid{};
    invalid.reason = static_cast<uint8_t>(RejectionReason::AuthenticationFailed);
    invalid.reserved[0] = 1;
    RejectionMessage ignored{};
    Check(!DecodeRejection(reinterpret_cast<const uint8_t*>(&invalid), sizeof(invalid), ignored),
          "non-zero reserved bytes rejected");
    Check(!DecodeRejection(reinterpret_cast<const uint8_t*>(&invalid), sizeof(invalid) - 1, ignored),
          "truncated envelope rejected");
    invalid.reserved[0] = 0;
    invalid.supportedClients = 0x80;
    Check(!DecodeRejection(reinterpret_cast<const uint8_t*>(&invalid), sizeof(invalid), ignored),
          "unknown supported-client bits rejected");
    Check(skr::RejectionForExitCode(0) == RejectionReason::None, "ordinary exit is not rejection");
    Check(skr::RejectionForExitCode(skr::kExitRestartLobby) == RejectionReason::None,
          "existing restart exit code preserved");

    std::cout << "SKR rejection tests: " << checks << " checks, 0 failures\n";
    return 0;
}
