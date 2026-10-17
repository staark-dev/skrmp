#pragma once
#include "updater/manifest.h"
#include <cstdint>

namespace skr::updater {

struct SemanticVersion {
    uint32_t major = 0;
    uint32_t minor = 0;
    uint32_t patch = 0;

    bool operator<(const SemanticVersion& o) const {
        if (major != o.major) return major < o.major;
        if (minor != o.minor) return minor < o.minor;
        return patch < o.patch;
    }
    bool operator==(const SemanticVersion& o) const {
        return major == o.major && minor == o.minor && patch == o.patch;
    }
    bool operator>=(const SemanticVersion& o) const {
        return !(*this < o);
    }
};

bool ParseVersion(const char* str, SemanticVersion& out);

enum class GateDecision : uint8_t {
    Allow,
    ForceUpdate,
    BlockUnsupported,
    BlockMaintenance
};

struct ChannelPolicy {
    Channel channel = Channel::Stable;
    SemanticVersion minVersion{};
    bool maintenanceMode = false;
    bool requireSignedManifest = true;
    bool requireAcReady = true;
};

class ReleaseGate {
public:
    void SetPolicy(const ChannelPolicy& policy) { policy_ = policy; }
    void SetCurrentVersion(const SemanticVersion& ver) { currentVersion_ = ver; }

    GateDecision Evaluate(const ReleaseManifest& manifest) const;

    bool IsVersionSupported(const SemanticVersion& ver) const;
    bool IsChannelAllowed(Channel ch) const;

    static Channel ParseChannel(const char* str);
    static const char* ChannelToString(Channel ch);

private:
    ChannelPolicy policy_;
    SemanticVersion currentVersion_{};
};

} // namespace skr::updater
