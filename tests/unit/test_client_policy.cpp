#include "client_policy.hpp"
#include "protocol/enhanced.h"

#include <cstdlib>
#include <iostream>
#include <string>

using skr::gate_omp::AdmissionDecision;
using skr::gate_omp::ClientPolicy;
using skr::gate_omp::WireClientType;

namespace {

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

bool Load(ClientPolicy& policy, const std::string& json, std::string& error) {
    error.clear();
    return policy.LoadFromJson(json, error);
}

} // namespace

int main() {
    const std::string target = R"json({
        "runtime": {"version": "1.0.23"},
        "client_policy": {
            "allow_standard_clients": true,
            "allow_enhanced_clients": true,
            "allowed_client_types": ["samp-0.3.7", "skr-enhanced", "openmp"],
            "allow_03dl": false,
            "require_enhanced_for": [],
            "enhanced_negotiation_timeout_ms": 10000
        }
    })json";

    ClientPolicy policy;
    std::string error;
    Check(Load(policy, target, error), "target policy parses");
    Check(policy.IsValid(), "target policy valid");
    Check(policy.Evaluate(WireClientType::Samp037) == AdmissionDecision::Allow, "0.3.7 allowed");
    Check(policy.Evaluate(WireClientType::OpenMp) == AdmissionDecision::Allow, "open.mp allowed");
    Check(policy.Evaluate(WireClientType::Samp03DL) == AdmissionDecision::DenySamp03DL, "0.3.DL denied");
    Check(policy.Evaluate(WireClientType::Unsupported) == AdmissionDecision::DenyUnsupported, "unknown denied");
    Check(policy.AdvertisedClients() == "SA-MP 0.3.7, SKR Client, open.mp", "advertising matches enforcement");
    Check(std::string(policy.ModeName()) == "hybrid", "target mode hybrid");

    const std::string legacy = R"json({"client_policy":{
        "allow_standard_clients":true,
        "allow_enhanced_clients":true,
        "require_enhanced_for":[],
        "enhanced_negotiation_timeout_ms":10000
    }})json";
    Check(Load(policy, legacy, error), "legacy policy parses");
    Check(policy.AllowsSamp037() && policy.AllowsOpenMp() && policy.AllowsEnhanced(), "legacy defaults preserved");
    Check(!policy.AllowsSamp03DL(), "legacy schema uses safe 0.3.DL default");

    const std::string enhancedOnly = R"json({"client_policy":{
        "allow_standard_clients":false,
        "allow_enhanced_clients":true,
        "allowed_client_types":["skr-enhanced"],
        "allow_03dl":false,
        "enhanced_negotiation_timeout_ms":4321
    }})json";
    Check(Load(policy, enhancedOnly, error), "enhanced-only policy parses");
    Check(policy.Evaluate(WireClientType::Samp037) == AdmissionDecision::RequireEnhanced,
          "compatible wire client held for enhanced negotiation");
    Check(policy.Evaluate(WireClientType::OpenMp) == AdmissionDecision::DenyOpenMp,
          "open.mp denied when not effective");
    Check(policy.EnhancedTimeoutMs() == 4321, "configured timeout consumed");

    skr::protocol::EnhancedSession session;
    Check(session.Begin(skr::protocol::kEnhancedVersion, skr::protocol::kEnhancedCapabilities,
                        7, 100, policy.EnhancedTimeoutMs()), "enhanced session begins");
    Check(session.deadline == 4421, "enhanced session uses configured timeout");

    const std::string dlAllowed = R"json({"client_policy":{
        "allow_standard_clients":true,
        "allow_enhanced_clients":false,
        "allowed_client_types":[],
        "allow_03dl":true,
        "enhanced_negotiation_timeout_ms":10000
    }})json";
    Check(Load(policy, dlAllowed, error), "explicit 0.3.DL policy parses");
    Check(policy.Evaluate(WireClientType::Samp03DL) == AdmissionDecision::Allow, "explicit 0.3.DL allowed");
    Check(policy.AdvertisedClients() == "SA-MP 0.3.DL", "0.3.DL advertisement exact");

    Check(!Load(policy, R"json({"client_policy":{"allowed_client_types":["samp-0.3.7","bogus"]}})json", error),
          "unknown allowlist entry rejected");
    Check(!Load(policy, R"json({"client_policy":{"allowed_client_types":["openmp","openmp"]}})json", error),
          "duplicate allowlist entry rejected");
    Check(!Load(policy, R"json({"client_policy":{"enhanced_negotiation_timeout_ms":999}})json", error),
          "unsafe timeout rejected");
    Check(!Load(policy, R"json({"client_policy":{"allow_standard_clients":false,"allow_enhanced_clients":false}})json", error),
          "empty effective policy rejected");
    Check(!Load(policy, R"json({"client_policy":{"require_enhanced_for":["example"]}})json", error),
          "unimplemented require_enhanced_for entries rejected");
    Check(!Load(policy, "{", error), "malformed JSON rejected");

    Check(!policy.LoadFromFile("this-policy-file-must-not-exist.json", error),
          "missing policy file fails closed");

    std::cout << "SKR client policy tests passed\n";
    return 0;
}
