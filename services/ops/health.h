#pragma once
#include <cstdint>
#include <cstring>
#include <mutex>

namespace skr::ops {

constexpr size_t kMaxComponentName = 32;
constexpr size_t kMaxComponents = 16;
constexpr size_t kMaxIncidents = 64;

enum class ComponentState : uint8_t {
    Healthy    = 0,
    Degraded   = 1,
    Down       = 2,
    Unknown    = 3
};

enum class IncidentSeverity : uint8_t {
    Info    = 0,
    Warning = 1,
    Error   = 2,
    Critical = 3
};

struct ComponentHealth {
    char name[kMaxComponentName]{};
    ComponentState state = ComponentState::Unknown;
    uint64_t lastCheckMs = 0;
    uint64_t uptimeMs = 0;
    uint32_t errorCount = 0;
    float latencyMs = 0.0f;
};

struct HealthSnapshot {
    ComponentHealth components[kMaxComponents]{};
    uint32_t componentCount = 0;
    uint64_t timestamp = 0;
    bool systemHealthy = false;
};

struct DiagnosticMetrics {
    uint64_t authRequestsTotal = 0;
    uint64_t authSuccessTotal = 0;
    uint64_t authFailureTotal = 0;
    uint64_t ticketsIssued = 0;
    uint64_t ticketsRedeemed = 0;
    uint64_t ticketsExpired = 0;
    uint64_t ticketsRevoked = 0;
    uint64_t acHeartbeatsReceived = 0;
    uint64_t acHeartbeatTimeouts = 0;
    uint64_t acFalsePositives = 0;
    float avgAuthLatencyMs = 0.0f;
    float peakAuthLatencyMs = 0.0f;
    uint64_t peakMemoryBytes = 0;
    uint64_t currentMemoryBytes = 0;
    uint64_t uptimeSeconds = 0;
};

class HealthMonitor {
public:
    void RegisterComponent(const char* name);
    void UpdateComponent(const char* name, ComponentState state,
                         float latencyMs = 0.0f);

    HealthSnapshot TakeSnapshot(uint64_t nowMs) const;

    void RecordAuthAttempt(bool success, float latencyMs);
    void RecordTicketIssued();
    void RecordTicketRedeemed();
    void RecordTicketExpired();
    void RecordTicketRevoked();
    void RecordAcHeartbeat();
    void RecordAcTimeout();
    void RecordAcFalsePositive();
    void UpdateMemoryUsage(uint64_t currentBytes);

    DiagnosticMetrics GetMetrics() const;
    void ResetMetrics();

    bool IsSystemHealthy() const;

private:
    mutable std::mutex mutex_;
    ComponentHealth components_[kMaxComponents]{};
    uint32_t componentCount_ = 0;
    DiagnosticMetrics metrics_{};
    uint64_t startTimeMs_ = 0;

    ComponentHealth* FindComponent(const char* name);
    const ComponentHealth* FindComponent(const char* name) const;
};

} // namespace skr::ops
