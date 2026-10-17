#include <cstdio>
#include <cstring>

#include "ops/health.h"
#include "ops/key_manager.h"
#include "ops/audit_log.h"
#include "ops/incident_response.h"
#include "crypto/crypto.h"

static int g_total = 0, g_passed = 0, g_failed = 0;

#define RUN_TEST(fn) do { \
    ++g_total; \
    printf("  [TEST] %-50s ", #fn); \
    if (fn()) { printf("PASS\n"); ++g_passed; } \
    else { printf("FAIL\n"); ++g_failed; } \
} while(0)

using namespace skr;

// ==========================================================
// Health monitor tests
// ==========================================================

static bool test_health_register_and_update() {
    ops::HealthMonitor mon;
    mon.RegisterComponent("auth");
    mon.RegisterComponent("gate");
    mon.UpdateComponent("auth", ops::ComponentState::Healthy, 1.5f);
    mon.UpdateComponent("gate", ops::ComponentState::Healthy, 2.0f);
    return mon.IsSystemHealthy();
}

static bool test_health_degraded_not_down() {
    ops::HealthMonitor mon;
    mon.RegisterComponent("auth");
    mon.UpdateComponent("auth", ops::ComponentState::Degraded);
    // Degraded is not Down, system should still be "healthy" (not down)
    return mon.IsSystemHealthy();
}

static bool test_health_down_is_unhealthy() {
    ops::HealthMonitor mon;
    mon.RegisterComponent("auth");
    mon.UpdateComponent("auth", ops::ComponentState::Down);
    return !mon.IsSystemHealthy();
}

static bool test_health_metrics_auth() {
    ops::HealthMonitor mon;
    mon.RecordAuthAttempt(true, 5.0f);
    mon.RecordAuthAttempt(true, 3.0f);
    mon.RecordAuthAttempt(false, 10.0f);
    auto m = mon.GetMetrics();
    return m.authRequestsTotal == 3 &&
           m.authSuccessTotal == 2 &&
           m.authFailureTotal == 1 &&
           m.peakAuthLatencyMs >= 9.9f;
}

static bool test_health_snapshot() {
    ops::HealthMonitor mon;
    mon.RegisterComponent("auth");
    mon.UpdateComponent("auth", ops::ComponentState::Healthy);
    auto snap = mon.TakeSnapshot(1000);
    return snap.componentCount == 1 && snap.systemHealthy;
}

// ==========================================================
// Key manager tests
// ==========================================================

static bool test_key_add_and_find() {
    crypto::MockSigningProvider mock;
    ops::KeyManager km(mock);

    uint8_t pub[crypto::kKeySize]{};
    memset(pub, 0xAA, crypto::kKeySize);
    if (!km.AddKey("key-001", ops::KeyPurpose::TicketSigning, pub, 2000000000))
        return false;

    auto* k = km.FindKey("key-001");
    return k && k->state == ops::KeyState::Active;
}

static bool test_key_revoke() {
    crypto::MockSigningProvider mock;
    ops::KeyManager km(mock);

    uint8_t pub[crypto::kKeySize]{};
    km.AddKey("key-001", ops::KeyPurpose::TicketSigning, pub, 2000000000);
    if (!km.RevokeKey("key-001", 1000)) return false;

    auto* k = km.FindKey("key-001");
    return k && k->state == ops::KeyState::Revoked;
}

static bool test_key_rotation() {
    crypto::MockSigningProvider mock;
    ops::KeyManager km(mock);

    uint8_t pub[crypto::kKeySize]{};
    km.AddKey("key-001", ops::KeyPurpose::ManifestSigning, pub, 2000000000);

    auto result = km.RotateKey("key-001", 1500000000);
    if (!result.success) return false;

    auto* oldK = km.FindKey("key-001");
    if (!oldK || oldK->state != ops::KeyState::Rotating) return false;

    auto* newK = km.FindKey(result.newKeyId);
    return newK && newK->state == ops::KeyState::Active;
}

static bool test_key_revoke_all_suspends() {
    crypto::MockSigningProvider mock;
    ops::KeyManager km(mock);

    uint8_t pub[crypto::kKeySize]{};
    km.AddKey("key-001", ops::KeyPurpose::TicketSigning, pub, 2000000000);
    km.AddKey("key-002", ops::KeyPurpose::ManifestSigning, pub, 2000000000);

    km.RevokeAll(1000);
    return km.IsIssuanceSuspended() &&
           km.FindKey("key-001")->state == ops::KeyState::Revoked &&
           km.FindKey("key-002")->state == ops::KeyState::Revoked;
}

static bool test_key_expire() {
    crypto::MockSigningProvider mock;
    ops::KeyManager km(mock);

    uint8_t pub[crypto::kKeySize]{};
    km.AddKey("key-old", ops::KeyPurpose::TicketSigning, pub, 1000);
    km.ExpireOldKeys(2000);

    auto* k = km.FindKey("key-old");
    return k && k->state == ops::KeyState::Expired;
}

// ==========================================================
// Audit log tests
// ==========================================================

static bool test_audit_record_and_query() {
    ops::AuditLog log;
    log.Record(ops::AuditCategory::Auth, "login", "user1", 42, 0,
               "successful login", true, 1000);
    log.Record(ops::AuditCategory::Ticket, "issue", "system", 42, 0,
               "ticket issued", true, 1001);

    return log.GetEntryCount() == 2;
}

static bool test_audit_query_by_category() {
    ops::AuditLog log;
    log.Record(ops::AuditCategory::Auth, "login", "u1", 1, 0, "", true, 100);
    log.Record(ops::AuditCategory::Key, "rotate", "admin", 0, 0, "", true, 101);
    log.Record(ops::AuditCategory::Auth, "logout", "u1", 1, 0, "", true, 102);

    ops::AuditEntry results[10]{};
    uint32_t count = log.QueryByCategory(ops::AuditCategory::Auth, results, 10);
    return count == 2;
}

static bool test_audit_query_by_account() {
    ops::AuditLog log;
    log.Record(ops::AuditCategory::Auth, "login", "u1", 42, 0, "", true, 100);
    log.Record(ops::AuditCategory::Auth, "login", "u2", 99, 0, "", true, 101);
    log.Record(ops::AuditCategory::Ticket, "issue", "sys", 42, 0, "", true, 102);

    ops::AuditEntry results[10]{};
    uint32_t count = log.QueryByAccount(42, results, 10);
    return count == 2;
}

static bool test_audit_circular_buffer() {
    ops::AuditLog log;
    // Fill beyond capacity
    for (uint32_t i = 0; i < ops::kMaxAuditEntries + 10; ++i) {
        log.Record(ops::AuditCategory::Auth, "event", "sys", i, 0, "", true, i);
    }
    return log.GetEntryCount() == ops::kMaxAuditEntries;
}

// ==========================================================
// Incident response tests
// ==========================================================

static bool test_incident_suspend_tickets() {
    crypto::MockSigningProvider mock;
    ops::HealthMonitor health;
    ops::KeyManager keys(mock);
    ops::AuditLog audit;
    ops::IncidentResponder responder(health, keys, audit);

    health.RegisterComponent("auth");
    health.RegisterComponent("gate");
    uint8_t pub[crypto::kKeySize]{};
    keys.AddKey("key-001", ops::KeyPurpose::TicketSigning, pub, 2000000000);

    auto report = responder.ExecuteAction(ops::IncidentAction::SuspendTickets,
                                          "compromised key", "admin", 1000);
    return report.executed && responder.IsInIncident() &&
           keys.IsIssuanceSuspended();
}

static bool test_incident_resolve() {
    crypto::MockSigningProvider mock;
    ops::HealthMonitor health;
    ops::KeyManager keys(mock);
    ops::AuditLog audit;
    ops::IncidentResponder responder(health, keys, audit);

    health.RegisterComponent("auth");
    responder.ExecuteAction(ops::IncidentAction::SuspendTickets,
                           "test", "admin", 1000);
    responder.ResolveIncident("admin", 2000);
    return !responder.IsInIncident() && !keys.IsIssuanceSuspended();
}

static bool test_incident_auto_check_high_failure() {
    crypto::MockSigningProvider mock;
    ops::HealthMonitor health;
    ops::KeyManager keys(mock);
    ops::AuditLog audit;
    ops::IncidentResponder responder(health, keys, audit);

    health.RegisterComponent("auth");
    uint8_t pub[crypto::kKeySize]{};
    keys.AddKey("key-001", ops::KeyPurpose::TicketSigning, pub, 2000000000);

    // Simulate >80% failure rate over 101+ requests
    for (int i = 0; i < 20; ++i)
        health.RecordAuthAttempt(true, 5.0f);
    for (int i = 0; i < 81; ++i)
        health.RecordAuthAttempt(false, 50.0f);

    auto report = responder.AutoCheck(5000);
    return report.executed && responder.IsInIncident();
}

// ==========================================================

int main() {
    printf("=== SKR Ops Unit Tests ===\n\n");

    printf("[Health Monitor]\n");
    RUN_TEST(test_health_register_and_update);
    RUN_TEST(test_health_degraded_not_down);
    RUN_TEST(test_health_down_is_unhealthy);
    RUN_TEST(test_health_metrics_auth);
    RUN_TEST(test_health_snapshot);

    printf("\n[Key Manager]\n");
    RUN_TEST(test_key_add_and_find);
    RUN_TEST(test_key_revoke);
    RUN_TEST(test_key_rotation);
    RUN_TEST(test_key_revoke_all_suspends);
    RUN_TEST(test_key_expire);

    printf("\n[Audit Log]\n");
    RUN_TEST(test_audit_record_and_query);
    RUN_TEST(test_audit_query_by_category);
    RUN_TEST(test_audit_query_by_account);
    RUN_TEST(test_audit_circular_buffer);

    printf("\n[Incident Response]\n");
    RUN_TEST(test_incident_suspend_tickets);
    RUN_TEST(test_incident_resolve);
    RUN_TEST(test_incident_auto_check_high_failure);

    printf("\n=== %d/%d passed", g_passed, g_total);
    if (g_failed > 0) printf(", %d FAILED", g_failed);
    printf(" ===\n");
    return g_failed > 0 ? 1 : 0;
}
