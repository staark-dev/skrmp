#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>

namespace skr::updater {

constexpr uint32_t kManifestVersion = 1;
constexpr size_t kSha256Size = 32;
constexpr size_t kMaxPathLen = 260;
constexpr size_t kMaxChannelLen = 16;
constexpr size_t kMaxPlatformLen = 16;
constexpr size_t kMaxKeyIdLen = 64;
constexpr size_t kMaxVersionLen = 32;

enum class Channel : uint8_t {
    Stable  = 0,
    Beta    = 1,
    Canary  = 2
};

enum class FileAction : uint8_t {
    Add     = 0,
    Update  = 1,
    Delete  = 2
};

struct ManifestEntry {
    char relativePath[kMaxPathLen]{};
    uint8_t sha256[kSha256Size]{};
    uint64_t sizeBytes = 0;
    FileAction action = FileAction::Add;
    bool executable = false;
};

struct ReleaseManifest {
    uint32_t manifestVersion = kManifestVersion;
    char version[kMaxVersionLen]{};
    char channel[kMaxChannelLen]{};
    char platform[kMaxPlatformLen]{};
    char architecture[kMaxPlatformLen]{};
    char keyId[kMaxKeyIdLen]{};
    uint64_t timestamp = 0;
    uint32_t minSupportedVersion[3]{};

    std::vector<ManifestEntry> entries;

    void Clear() {
        manifestVersion = kManifestVersion;
        memset(version, 0, sizeof(version));
        memset(channel, 0, sizeof(channel));
        memset(platform, 0, sizeof(platform));
        memset(architecture, 0, sizeof(architecture));
        memset(keyId, 0, sizeof(keyId));
        timestamp = 0;
        memset(minSupportedVersion, 0, sizeof(minSupportedVersion));
        entries.clear();
    }
};

// Simple manifest serialization (production would use JSON + signature)
class ManifestSerializer {
public:
    static bool Serialize(const ReleaseManifest& manifest,
                          std::vector<uint8_t>& out);

    static bool Deserialize(const uint8_t* data, size_t len,
                            ReleaseManifest& out);

    static bool ValidateEntry(const ManifestEntry& entry);

private:
    static bool IsPathSafe(const char* path);
};

} // namespace skr::updater
