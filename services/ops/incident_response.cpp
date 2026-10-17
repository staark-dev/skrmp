#include "incident_response.h"
#include <cstring>

namespace skr::ops {

IncidentResponder::IncidentResponder(HealthMonitor& health, KeyManager& keys,
                                     AuditLog& audit)
    : health_(health), keys_(keys), audit_(audit) {}

IncidentReport IncidentResponder::ExecuteAction(IncidentAction action,
                                                 const char* reason,
                                                 const char* actorId,
                                                 uint64_t nowTimestamp) {
    IncidentReport report{};
    report.timestamp = nowTimestamp;
    report.action = action;
    if (reason) strncpy(report.reason, reason, sizeof(report.reason) - 1);

    switch (action) {
        case IncidentAction::SuspendTickets:
            keys_.RevokeAll(nowTimestamp);
            // issuanceSuspended_ is now true in KeyManager
            audit_.Record(AuditCategory::Ops, "suspend_tickets",
                         actorId, 0, 0, reason, true, nowTimestamp);
            report.executed = true;
            inIncident_ = true;
            incidentStarted_ = nowTimestamp;
            break;

        case IncidentAction::RevokeAllKeys:
            keys_.RevokeAll(nowTimestamp);
            audit_.Record(AuditCategory::Key, "revoke_all_keys",
                         actorId, 0, 0, reason, true, nowTimestamp);
            report.executed = true;
            inIncident_ = true;
            incidentStarted_ = nowTimestamp;
            break;

        case IncidentAction::ForceMinVersion:
            audit_.Record(AuditCategory::Ops, "force_min_version",
                         actorId, 0, 0, reason, true, nowTimestamp);
            report.executed = true;
            break;

        case IncidentAction::FullShutdown:
            keys_.RevokeAll(nowTimestamp);
            health_.UpdateComponent("auth", ComponentState::Down);
            health_.UpdateComponent("gate", ComponentState::Down);
            audit_.Record(AuditCategory::Ops, "full_shutdown",
                         actorId, 0, 0, reason, true, nowTimestamp);
            report.executed = true;
            inIncident_ = true;
            incidentStarted_ = nowTimestamp;
            break;

        case IncidentAction::None:
            break;
    }

    return report;
}

IncidentReport IncidentResponder::AutoCheck(uint64_t nowTimestamp) {
    IncidentReport report{};
    report.timestamp = nowTimestamp;

    if (inIncident_) return report;

    auto metrics = health_.GetMetrics();

    // If auth failure rate exceeds 80% over significant volume, suspend
    if (metrics.authRequestsTotal > 100) {
        float failRate = static_cast<float>(metrics.authFailureTotal) /
                         static_cast<float>(metrics.authRequestsTotal);
        if (failRate > 0.8f) {
            return ExecuteAction(IncidentAction::SuspendTickets,
                                "auto: auth failure rate >80%",
                                "system", nowTimestamp);
        }
    }

    // If AC false positive rate is too high, log warning
    if (metrics.acHeartbeatsReceived > 50 && metrics.acFalsePositives > 0) {
        float fpRate = static_cast<float>(metrics.acFalsePositives) /
                       static_cast<float>(metrics.acHeartbeatsReceived);
        if (fpRate > 0.1f) {
            audit_.Record(AuditCategory::Ac, "high_false_positive_rate",
                         "system", 0, 0,
                         "auto: AC false positive rate >10%",
                         false, nowTimestamp);
        }
    }

    // Expire old keys
    keys_.ExpireOldKeys(nowTimestamp);

    return report;
}

void IncidentResponder::ResolveIncident(const char* actorId,
                                         uint64_t nowTimestamp) {
    if (!inIncident_) return;

    keys_.ResumeIssuance();
    health_.UpdateComponent("auth", ComponentState::Healthy);
    health_.UpdateComponent("gate", ComponentState::Healthy);

    audit_.Record(AuditCategory::Ops, "incident_resolved",
                 actorId, 0, 0, "manual resolution", true, nowTimestamp);

    inIncident_ = false;
    incidentStarted_ = 0;
}

} // namespace skr::ops
