#include "skrac/ac_policy.h"
#include <cstring>
#include <cstdio>

namespace skr::ac {

AcPolicyEnforcer::AcPolicyEnforcer(IAcAdapter& adapter, const PolicyConfig& config)
    : adapter_(adapter), config_(config) {}

bool AcPolicyEnforcer::Initialize(const char* sessionId) {
    if (!sessionId || sessionId[0] == '\0') return false;

    if (config_.mode == AcEnforcement::Production && adapter_.IsMock()) {
        strncpy(lastReason_, "mock adapter rejected in production", kMaxReasonLen - 1);
        return false;
    }

    strncpy(sessionId_, sessionId, kMaxSessionIdLen - 1);
    initAttempts_ = 0;
    consecutiveFailures_ = 0;

    return TryInit();
}

bool AcPolicyEnforcer::TryInit() {
    ++initAttempts_;
    if (!adapter_.Start(sessionId_)) {
        snprintf(lastReason_, kMaxReasonLen, "adapter init failed (attempt %u)", initAttempts_);
        lastState_ = AcState::Failed;
        return false;
    }

    initialized_ = true;
    lastState_ = AcState::Starting;
    initStartMs_ = 0;
    return true;
}

PolicyAction AcPolicyEnforcer::Tick(uint64_t nowMs) {
    if (!initialized_) {
        if (initAttempts_ < config_.initRetryLimit) {
            if (TryInit()) return PolicyAction::Continue;
        }
        return PolicyAction::Disconnect;
    }

    if (initStartMs_ == 0) initStartMs_ = nowMs;

    AcStatusResult status{};
    bool pollOk = adapter_.PollStatus(status);

    if (!pollOk) {
        ++consecutiveFailures_;
        snprintf(lastReason_, kMaxReasonLen,
                 "poll failed (%u consecutive)", consecutiveFailures_);

        if (consecutiveFailures_ >= config_.maxConsecutiveFailures)
            return PolicyAction::Disconnect;
        return PolicyAction::Warn;
    }

    consecutiveFailures_ = 0;
    lastState_ = status.state;
    lastPollMs_ = nowMs;

    AcHeartbeat beat{};
    if (adapter_.GetHeartbeat(beat)) {
        lastHeartbeatSeq_ = beat.sequence;
        lastHeartbeatMs_ = nowMs;
    }

    switch (lastState_) {
        case AcState::Ready:     return HandleReady(nowMs);
        case AcState::Degraded:  return HandleDegraded(nowMs);
        case AcState::Failed:    return HandleFailed();
        case AcState::Starting:  return HandleStarting(nowMs);
        case AcState::Unknown:
        default:                 return PolicyAction::Disconnect;
    }
}

PolicyAction AcPolicyEnforcer::HandleReady(uint64_t nowMs) {
    degradedSinceMs_ = 0;

    if (lastHeartbeatMs_ > 0 &&
        (nowMs - lastHeartbeatMs_) > config_.heartbeatTimeoutMs) {
        strncpy(lastReason_, "heartbeat timeout while ready", kMaxReasonLen - 1);
        return PolicyAction::Warn;
    }
    return PolicyAction::Continue;
}

PolicyAction AcPolicyEnforcer::HandleDegraded(uint64_t nowMs) {
    if (degradedSinceMs_ == 0) degradedSinceMs_ = nowMs;

    uint64_t elapsed = nowMs - degradedSinceMs_;
    if (elapsed > config_.degradedGraceMs) {
        strncpy(lastReason_, "degraded state exceeded grace period", kMaxReasonLen - 1);
        return PolicyAction::Disconnect;
    }
    snprintf(lastReason_, kMaxReasonLen,
             "degraded for %llums (grace %ums)",
             static_cast<unsigned long long>(elapsed),
             config_.degradedGraceMs);
    return PolicyAction::Warn;
}

PolicyAction AcPolicyEnforcer::HandleFailed() {
    strncpy(lastReason_, "AC engine reported failure", kMaxReasonLen - 1);

    if (initAttempts_ < config_.initRetryLimit) {
        adapter_.Stop();
        initialized_ = false;
        return PolicyAction::RetryInit;
    }
    return PolicyAction::Disconnect;
}

PolicyAction AcPolicyEnforcer::HandleStarting(uint64_t nowMs) {
    uint64_t elapsed = nowMs - initStartMs_;
    if (elapsed > config_.heartbeatTimeoutMs * 2) {
        strncpy(lastReason_, "AC engine stuck in starting state", kMaxReasonLen - 1);
        return PolicyAction::Disconnect;
    }
    return PolicyAction::Continue;
}

bool AcPolicyEnforcer::IsOperational() const noexcept {
    return initialized_ &&
           (lastState_ == AcState::Ready || lastState_ == AcState::Degraded);
}

void AcPolicyEnforcer::Shutdown() noexcept {
    if (initialized_) {
        adapter_.Stop();
        initialized_ = false;
    }
    lastState_ = AcState::Unknown;
    consecutiveFailures_ = 0;
    lastReason_[0] = '\0';
}

} // namespace skr::ac
