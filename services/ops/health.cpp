#include "health.h"
#include <cstring>

namespace skr::ops {

ComponentHealth* HealthMonitor::FindComponent(const char* name) {
    for (uint32_t i = 0; i < componentCount_; ++i) {
        if (strcmp(components_[i].name, name) == 0)
            return &components_[i];
    }
    return nullptr;
}

const ComponentHealth* HealthMonitor::FindComponent(const char* name) const {
    for (uint32_t i = 0; i < componentCount_; ++i) {
        if (strcmp(components_[i].name, name) == 0)
            return &components_[i];
    }
    return nullptr;
}

void HealthMonitor::RegisterComponent(const char* name) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (componentCount_ >= kMaxComponents) return;
    if (FindComponent(name)) return;

    auto& c = components_[componentCount_++];
    strncpy(c.name, name, kMaxComponentName - 1);
    c.state = ComponentState::Unknown;
}

void HealthMonitor::UpdateComponent(const char* name, ComponentState state,
                                    float latencyMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto* c = FindComponent(name);
    if (!c) return;

    c->state = state;
    c->latencyMs = latencyMs;
    if (state != ComponentState::Healthy)
        ++c->errorCount;
}

HealthSnapshot HealthMonitor::TakeSnapshot(uint64_t nowMs) const {
    std::lock_guard<std::mutex> lock(mutex_);
    HealthSnapshot snap{};
    snap.timestamp = nowMs;
    snap.componentCount = componentCount_;
    bool allHealthy = true;

    for (uint32_t i = 0; i < componentCount_; ++i) {
        snap.components[i] = components_[i];
        snap.components[i].lastCheckMs = nowMs;
        if (components_[i].state != ComponentState::Healthy)
            allHealthy = false;
    }
    snap.systemHealthy = allHealthy && componentCount_ > 0;
    return snap;
}

void HealthMonitor::RecordAuthAttempt(bool success, float latencyMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++metrics_.authRequestsTotal;
    if (success) ++metrics_.authSuccessTotal;
    else ++metrics_.authFailureTotal;

    // Running average
    float total = static_cast<float>(metrics_.authRequestsTotal);
    metrics_.avgAuthLatencyMs =
        metrics_.avgAuthLatencyMs * ((total - 1.0f) / total) +
        latencyMs / total;

    if (latencyMs > metrics_.peakAuthLatencyMs)
        metrics_.peakAuthLatencyMs = latencyMs;
}

void HealthMonitor::RecordTicketIssued() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++metrics_.ticketsIssued;
}

void HealthMonitor::RecordTicketRedeemed() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++metrics_.ticketsRedeemed;
}

void HealthMonitor::RecordTicketExpired() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++metrics_.ticketsExpired;
}

void HealthMonitor::RecordTicketRevoked() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++metrics_.ticketsRevoked;
}

void HealthMonitor::RecordAcHeartbeat() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++metrics_.acHeartbeatsReceived;
}

void HealthMonitor::RecordAcTimeout() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++metrics_.acHeartbeatTimeouts;
}

void HealthMonitor::RecordAcFalsePositive() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++metrics_.acFalsePositives;
}

void HealthMonitor::UpdateMemoryUsage(uint64_t currentBytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    metrics_.currentMemoryBytes = currentBytes;
    if (currentBytes > metrics_.peakMemoryBytes)
        metrics_.peakMemoryBytes = currentBytes;
}

DiagnosticMetrics HealthMonitor::GetMetrics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return metrics_;
}

void HealthMonitor::ResetMetrics() {
    std::lock_guard<std::mutex> lock(mutex_);
    metrics_ = {};
}

bool HealthMonitor::IsSystemHealthy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (componentCount_ == 0) return false;
    for (uint32_t i = 0; i < componentCount_; ++i) {
        if (components_[i].state == ComponentState::Down)
            return false;
    }
    return true;
}

} // namespace skr::ops
