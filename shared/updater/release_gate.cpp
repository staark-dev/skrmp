#include "updater/release_gate.h"
#include <cstring>
#include <cstdlib>

namespace skr::updater {

bool ParseVersion(const char* str, SemanticVersion& out) {
    if (!str) return false;
    out = {};

    char* end = nullptr;
    out.major = static_cast<uint32_t>(strtoul(str, &end, 10));
    if (!end || *end != '.') return false;
    str = end + 1;

    out.minor = static_cast<uint32_t>(strtoul(str, &end, 10));
    if (!end || *end != '.') return false;
    str = end + 1;

    out.patch = static_cast<uint32_t>(strtoul(str, &end, 10));
    if (end == str) return false;

    return true;
}

Channel ReleaseGate::ParseChannel(const char* str) {
    if (!str) return Channel::Stable;
    if (strcmp(str, "beta") == 0) return Channel::Beta;
    if (strcmp(str, "canary") == 0) return Channel::Canary;
    return Channel::Stable;
}

const char* ReleaseGate::ChannelToString(Channel ch) {
    switch (ch) {
        case Channel::Beta:   return "beta";
        case Channel::Canary: return "canary";
        default:              return "stable";
    }
}

GateDecision ReleaseGate::Evaluate(const ReleaseManifest& manifest) const {
    if (policy_.maintenanceMode)
        return GateDecision::BlockMaintenance;

    Channel manifestChannel = ParseChannel(manifest.channel);
    if (!IsChannelAllowed(manifestChannel))
        return GateDecision::BlockUnsupported;

    SemanticVersion manifestVer{};
    if (!ParseVersion(manifest.version, manifestVer))
        return GateDecision::BlockUnsupported;

    // Check manifest's minimum supported version against our current version
    SemanticVersion minFromManifest{};
    minFromManifest.major = manifest.minSupportedVersion[0];
    minFromManifest.minor = manifest.minSupportedVersion[1];
    minFromManifest.patch = manifest.minSupportedVersion[2];

    if (currentVersion_.major > 0 || currentVersion_.minor > 0 || currentVersion_.patch > 0) {
        if (currentVersion_ < minFromManifest)
            return GateDecision::ForceUpdate;
    }

    if (!IsVersionSupported(currentVersion_))
        return GateDecision::ForceUpdate;

    return GateDecision::Allow;
}

bool ReleaseGate::IsVersionSupported(const SemanticVersion& ver) const {
    return ver >= policy_.minVersion;
}

bool ReleaseGate::IsChannelAllowed(Channel ch) const {
    // Canary allows all; beta allows beta+stable; stable allows only stable
    switch (policy_.channel) {
        case Channel::Canary: return true;
        case Channel::Beta:   return ch == Channel::Beta || ch == Channel::Stable;
        case Channel::Stable: return ch == Channel::Stable;
    }
    return false;
}

} // namespace skr::updater
