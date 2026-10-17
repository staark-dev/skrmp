#pragma once
#include <cstdint>
#include <cstring>

namespace skr::compat {

constexpr size_t kSha256Len = 32;
constexpr size_t kMaxHashHex = 65;
constexpr size_t kMaxPathLen = 260;
constexpr size_t kMaxVersionStr = 32;
constexpr size_t kMaxModuleEntries = 16;
constexpr size_t kMaxTestEntries = 32;

enum class ProfileStatus : uint8_t {
    Unchecked   = 0,
    Pending     = 1,
    Certified   = 2,
    Failed      = 3,
    Unsupported = 4
};

enum class TestResult : uint8_t {
    NotRun   = 0,
    Passed   = 1,
    Failed   = 2,
    Skipped  = 3
};

struct ModuleHash {
    char name[64]{};
    char path[kMaxPathLen]{};
    uint8_t sha256[kSha256Len]{};
    uint64_t sizeBytes = 0;
    char fileVersion[kMaxVersionStr]{};
    bool verified = false;
};

struct TestEntry {
    char name[64]{};
    char category[32]{};
    TestResult result = TestResult::NotRun;
    uint32_t iterations = 0;
    uint32_t failures = 0;
    char notes[128]{};
};

struct CompatibilityProfile {
    uint32_t version = 1;
    char targetConfig[64]{};         // e.g. "samp-037-r2"
    char clientBuild[kMaxVersionStr]{};
    ProfileStatus status = ProfileStatus::Unchecked;

    // System info
    char osVersion[64]{};
    char cpuName[64]{};
    char gpuName[64]{};
    char gpuDriver[32]{};

    // Module hashes
    ModuleHash modules[kMaxModuleEntries]{};
    uint32_t moduleCount = 0;

    // Test results
    TestEntry tests[kMaxTestEntries]{};
    uint32_t testCount = 0;

    // Timestamps
    uint64_t createdAt = 0;
    uint64_t lastUpdated = 0;

    void Clear() {
        memset(this, 0, sizeof(*this));
        version = 1;
    }
};

class ProfileBuilder {
public:
    ProfileBuilder();

    void SetTarget(const char* config, const char* clientBuild);
    void CollectSystemInfo();
    bool AddModuleHash(const char* name, const char* path);
    void AddTestResult(const char* name, const char* category,
                       TestResult result, uint32_t iterations = 1,
                       uint32_t failures = 0, const char* notes = nullptr);

    void EvaluateStatus();

    const CompatibilityProfile& GetProfile() const { return profile_; }

    bool SerializeToFile(const wchar_t* path) const;
    bool DeserializeFromFile(const wchar_t* path);

private:
    CompatibilityProfile profile_;
};

// Convert SHA-256 bytes to hex string
void Sha256ToHex(const uint8_t hash[kSha256Len], char out[kMaxHashHex]);

} // namespace skr::compat
