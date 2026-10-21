#pragma once

#include <cstdint>
#include <string>
#include <vector>

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
};

const char* AdmissionReason(AdmissionDecision decision);

} // namespace skr::gate_omp
