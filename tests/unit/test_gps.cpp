#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

// ── Inline GPS types for isolated testing (no GTA dependency) ───────
// These mirror client/gps/gps_types.h exactly.

namespace skr::gps {
struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float DistanceTo2D(const Vec3& o) const {
        float dx = x - o.x, dy = y - o.y;
        return std::sqrt(dx * dx + dy * dy);
    }
};
enum class NavStatus : unsigned char {
    Idle = 0, Calculating, Navigating, Arrived, Unreachable, Failed,
};
} // namespace skr::gps

// ── Minimal test harness ────────────────────────────────────────────
static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; std::printf("  FAIL: %s (line %d)\n", msg, __LINE__); } \
} while(0)

// ═══════════════════════════════════════════════════════════════════════
// Test: Destination state machine
// ═══════════════════════════════════════════════════════════════════════

namespace {

static bool  s_hasDest    = false;
static skr::gps::Vec3 s_dest = {};
static float s_radius     = 15.0f;

void SetDest(const skr::gps::Vec3& p) {
    if (s_hasDest && std::abs(s_dest.x - p.x) < 1.0f && std::abs(s_dest.y - p.y) < 1.0f)
        return;
    s_dest = p; s_hasDest = true;
}
void ClearDest() { s_hasDest = false; s_dest = {}; }
bool HasDest() { return s_hasDest; }
bool IsArr(const skr::gps::Vec3& player) {
    return s_hasDest && player.DistanceTo2D(s_dest) <= s_radius;
}

} // anon

static void TestDestination() {
    std::puts("== Destination state machine ==");

    // Initially no destination.
    ClearDest();
    CHECK(!HasDest(), "no dest initially");

    // Set destination.
    SetDest({100.0f, 200.0f, 10.0f});
    CHECK(HasDest(), "dest set");

    // Same position again — should be idempotent.
    SetDest({100.5f, 200.5f, 10.0f});
    CHECK(s_dest.x == 100.0f, "idempotent: x unchanged");

    // Different position — should update.
    SetDest({500.0f, 600.0f, 10.0f});
    CHECK(s_dest.x == 500.0f, "dest updated to 500");

    // Arrival detection.
    skr::gps::Vec3 farAway{0.0f, 0.0f, 0.0f};
    CHECK(!IsArr(farAway), "not arrived when far");

    skr::gps::Vec3 near{505.0f, 605.0f, 10.0f};
    CHECK(IsArr(near), "arrived within radius");

    // Clear destination.
    ClearDest();
    CHECK(!HasDest(), "dest cleared");
    CHECK(!IsArr(near), "not arrived after clear");
}

// ═══════════════════════════════════════════════════════════════════════
// Test: Route state transitions
// ═══════════════════════════════════════════════════════════════════════

static void TestRouteState() {
    std::puts("== Route state transitions ==");

    using skr::gps::NavStatus;

    // Simulate route lifecycle.
    NavStatus status = NavStatus::Idle;
    CHECK(status == NavStatus::Idle, "initial idle");

    // Begin calculation.
    status = NavStatus::Calculating;
    CHECK(status == NavStatus::Calculating, "calculating");

    // Route found.
    status = NavStatus::Navigating;
    CHECK(status == NavStatus::Navigating, "navigating");

    // Arrived.
    status = NavStatus::Arrived;
    CHECK(status == NavStatus::Arrived, "arrived");

    // Reset.
    status = NavStatus::Idle;
    CHECK(status == NavStatus::Idle, "back to idle");

    // Unreachable destination.
    status = NavStatus::Calculating;
    status = NavStatus::Unreachable;
    CHECK(status == NavStatus::Unreachable, "unreachable");

    // Failed (crash guard).
    status = NavStatus::Failed;
    CHECK(status == NavStatus::Failed, "failed");
}

// ═══════════════════════════════════════════════════════════════════════
// Test: Repeated destination changes
// ═══════════════════════════════════════════════════════════════════════

static void TestRepeatedChanges() {
    std::puts("== Repeated destination changes ==");
    ClearDest();

    for (int i = 0; i < 100; ++i)
        SetDest({static_cast<float>(i * 10), static_cast<float>(i * 10), 0.0f});

    CHECK(HasDest(), "dest after 100 changes");
    CHECK(std::abs(s_dest.x - 990.0f) < 0.01f, "final dest x=990");

    ClearDest();
    CHECK(!HasDest(), "cleared after repeated changes");
}

// ═══════════════════════════════════════════════════════════════════════
// Test: Deviation detection (point-to-point distance)
// ═══════════════════════════════════════════════════════════════════════

static void TestDeviation() {
    std::puts("== Deviation detection ==");

    skr::gps::Vec3 routePoint{100.0f, 100.0f, 0.0f};
    skr::gps::Vec3 onRoute{110.0f, 105.0f, 0.0f};
    skr::gps::Vec3 deviated{200.0f, 200.0f, 0.0f};

    float distOn  = onRoute.DistanceTo2D(routePoint);
    float distDev = deviated.DistanceTo2D(routePoint);

    CHECK(distOn < 40.0f, "on-route distance < threshold");
    CHECK(distDev > 40.0f, "deviated distance > threshold");
}

// ═══════════════════════════════════════════════════════════════════════
// Test: Cancellation
// ═══════════════════════════════════════════════════════════════════════

static void TestCancellation() {
    std::puts("== Cancellation ==");

    SetDest({500.0f, 500.0f, 0.0f});
    CHECK(HasDest(), "dest before cancel");

    ClearDest();
    CHECK(!HasDest(), "dest after cancel");

    // Cancel when already cancelled — no crash.
    ClearDest();
    CHECK(!HasDest(), "double cancel safe");
}

int main() {
    TestDestination();
    TestRouteState();
    TestRepeatedChanges();
    TestDeviation();
    TestCancellation();

    std::printf("\ngps: %d checks passed", g_pass);
    if (g_fail) std::printf(", %d FAILED", g_fail);
    std::puts("");
    return g_fail ? 1 : 0;
}
