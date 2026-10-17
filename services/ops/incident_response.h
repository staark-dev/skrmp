#pragma once
#include "health.h"
#include "key_manager.h"
#include "audit_log.h"
#include <cstdint>

namespace skr::ops {

enum class IncidentAction : uint8_t {
    None           = 0,
    SuspendTickets = 1,
    RevokeAllKeys  = 2,
    ForceMinVersion = 3,
    FullShutdown   = 4
};

struct IncidentReport {
    uint64_t timestamp = 0;
    IncidentAction action = IncidentAction::None;
    char reason[128]{};
    bool executed = false;
};

class IncidentResponder {
public:
    IncidentResponder(HealthMonitor& health, KeyManager& keys, AuditLog& audit);

    IncidentReport ExecuteAction(IncidentAction action, const char* reason,
                                 const char* actorId, uint64_t nowTimestamp);

    // Automated checks based on health state
    IncidentReport AutoCheck(uint64_t nowTimestamp);

    bool IsInIncident() const { return inIncident_; }
    void ResolveIncident(const char* actorId, uint64_t nowTimestamp);

private:
    HealthMonitor& health_;
    KeyManager& keys_;
    AuditLog& audit_;
    bool inIncident_ = false;
    uint64_t incidentStarted_ = 0;
};

} // namespace skr::ops
