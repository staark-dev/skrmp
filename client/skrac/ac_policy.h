#pragma once
#include "skrac/ac_types.h"
#include "skrac/mock_adapter.h"
#include <cstdint>

namespace skr::ac {

enum class AcEnforcement : uint8_t {
    Development,
    Production
};

enum class PolicyAction : uint8_t {
    Continue,
    Warn,
    Disconnect,
    RetryInit
};

struct PolicyConfig {
    AcEnforcement mode = AcEnforcement::Development;
    uint32_t heartbeatIntervalMs = 5000;
    uint32_t heartbeatTimeoutMs = 15000;
    uint32_t maxConsecutiveFailures = 3;
    uint32_t initRetryLimit = 2;
    uint32_t degradedGraceMs = 30000;
};

class AcPolicyEnforcer {
public:
    explicit AcPolicyEnforcer(IAcAdapter& adapter, const PolicyConfig& config = {});

    bool Initialize(const char* sessionId);

    PolicyAction Tick(uint64_t nowMs);

    AcState GetCurrentState() const noexcept { return lastState_; }
    uint32_t GetHeartbeatSeq() const noexcept { return lastHeartbeatSeq_; }
    bool IsOperational() const noexcept;
    const char* GetLastReason() const noexcept { return lastReason_; }

    void Shutdown() noexcept;

private:
    bool TryInit();
    PolicyAction HandleReady(uint64_t nowMs);
    PolicyAction HandleDegraded(uint64_t nowMs);
    PolicyAction HandleFailed();
    PolicyAction HandleStarting(uint64_t nowMs);

    IAcAdapter& adapter_;
    PolicyConfig config_;

    AcState lastState_ = AcState::Unknown;
    uint32_t lastHeartbeatSeq_ = 0;
    uint64_t lastHeartbeatMs_ = 0;
    uint64_t lastPollMs_ = 0;
    uint64_t degradedSinceMs_ = 0;
    uint64_t initStartMs_ = 0;
    uint32_t consecutiveFailures_ = 0;
    uint32_t initAttempts_ = 0;
    bool initialized_ = false;
    char lastReason_[kMaxReasonLen]{};
    char sessionId_[kMaxSessionIdLen]{};
};

} // namespace skr::ac
