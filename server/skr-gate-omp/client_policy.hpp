#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "protocol/radar_style.h"

namespace skr::gate_omp {

enum class WireClientType : uint8_t {
    Samp037,
    Samp03DL,
    OpenMp,
    Unsupported
};

enum class AdmissionDecision : uint8_t {
    Allow,
    RequireEnhanced,
    DenySamp037,
    DenySamp03DL,
    DenyOpenMp,
    DenyEnhanced,
    DenyUnsupported,
    DenyInvalidPolicy
};

struct ServerArtwork {
    std::string logoUrl;
    std::string bannerUrl;
    bool HasArtwork() const { return !logoUrl.empty() || !bannerUrl.empty(); }
};

// Parsed from the "hud" section of skr-runtime.json.
// Provides server-wide defaults applied to every new player connection.
struct HudConfig {
    protocol::RadarStyle defaultRadarStyle = protocol::RadarStyle::Round;
    protocol::RadarSizeMode defaultRadarSize = protocol::RadarSizeMode::Default;
    float defaultRadarScale = 1.0f;
    bool enableSpeedometer = true;
    bool enableMinimap = true;
    bool enablePhone = true;
};

class ClientPolicy {
public:
    bool LoadFromFile(const char* path, std::string& error);
    bool LoadFromJson(const std::string& json, std::string& error);

    bool IsValid() const { return valid_; }
    bool AllowsEnhanced() const { return allowEnhanced_; }
    uint64_t EnhancedTimeoutMs() const { return enhancedTimeoutMs_; }
    AdmissionDecision Evaluate(WireClientType type) const;

    std::string AdvertisedClients() const;
    const char* ModeName() const;
    const std::vector<std::string>& Warnings() const { return warnings_; }

    bool AllowsSamp037() const { return allowSamp037_; }
    bool AllowsSamp03DL() const { return allowSamp03DL_; }
    bool AllowsOpenMp() const { return allowOpenMp_; }

    const ServerArtwork& Artwork() const { return artwork_; }
    const HudConfig& Hud() const { return hud_; }

private:
    void ResetDefaults();

    bool valid_ = false;
    bool allowSamp037_ = true;
    bool allowEnhanced_ = true;
    bool allowOpenMp_ = true;
    bool allowSamp03DL_ = false;
    uint64_t enhancedTimeoutMs_ = 10000;
    std::vector<std::string> warnings_;
    ServerArtwork artwork_;
    HudConfig hud_;
};

const char* AdmissionReason(AdmissionDecision decision);

} // namespace skr::gate_omp
