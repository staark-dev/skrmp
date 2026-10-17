#pragma once
#include <cstdint>

namespace skr::ac {

enum class AcState : uint8_t {
    Unknown   = 0,
    Starting  = 1,
    Ready     = 2,
    Degraded  = 3,
    Failed    = 4
};

enum class AcVerdict : uint8_t {
    Clean     = 0,
    Suspicious = 1,
    Detected  = 2,
    Error     = 3
};

constexpr uint32_t kAcApiVersion = 1;
constexpr uint32_t kMaxSessionIdLen = 64;
constexpr uint32_t kMaxVersionLen = 32;
constexpr uint32_t kMaxReasonLen = 128;

#pragma pack(push, 1)

struct AcInitParams {
    uint32_t structSize;
    uint32_t apiVersion;
    char sessionId[kMaxSessionIdLen];
    uint32_t flags;
};

struct AcStatusResult {
    uint32_t structSize;
    AcState state;
    char engineVersion[kMaxVersionLen];
    char policyVersion[kMaxVersionLen];
    uint64_t uptimeMs;
    uint32_t heartbeatSeq;
};

struct AcHeartbeat {
    uint32_t structSize;
    uint32_t sequence;
    uint64_t timestampMs;
    AcState state;
    AcVerdict lastVerdict;
    char reason[kMaxReasonLen];
};

#pragma pack(pop)

// C ABI function signatures for external SKRAC DLL
extern "C" {
    using SkracInitFn    = int32_t(__cdecl*)(const AcInitParams* params);
    using SkracPollFn    = int32_t(__cdecl*)(AcStatusResult* out);
    using SkracBeatFn    = int32_t(__cdecl*)(AcHeartbeat* out);
    using SkracStopFn    = void(__cdecl*)();
    using SkracVersionFn = uint32_t(__cdecl*)();
}

} // namespace skr::ac
