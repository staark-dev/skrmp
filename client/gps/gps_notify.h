#pragma once
// ═══════════════════════════════════════════════════════════════════════
// GPS navigation events → notification adapter (API-003.2, Part C)
// ═══════════════════════════════════════════════════════════════════════
// Turns gps_feedback events into a few important notifications (route set,
// rerouted, arrived, unavailable). Continuous telemetry (distance) stays in
// the navigation panel. Policy is pure and unit-tested; delivery goes through
// a Sink so this module never owns UI.
//
// MERGE POINT (codex/c3-native-api-ui): this tree has no C3 renderer, so no
// sink is installed and accepted notices are only counted
// (BLOCKED — C3 NOT IN CURRENT TREE). The sink is ready in gps_notify_c3.cpp;
// after C3 is merged, configure with -DSKR_GPS_C3_BRIDGE=ON and Install()
// connects it. See docs/GPS_C3_INTEGRATION.md.
// No Pawn/Gate protocol involvement: these are client-local notices.

#include "gps_feedback.h"
#include <cstdint>

namespace skr::gps::notify {

enum class Kind : uint8_t { Navigation, Success, Error };

struct Notice {
    Kind    kind = Kind::Navigation;
    uint8_t durationSec = 4;
    char    text[96] = {};
};

// Rate limits / coalescing (ms).
constexpr uint32_t kStartCoalesceMs  = 4000;    // waypoint moved repeatedly
constexpr uint32_t kRerouteMinGapMs  = 20000;   // reroutes: at most one notice per 20 s
constexpr uint32_t kArrivedDedupeMs  = 5000;
constexpr uint32_t kFailureMinGapMs  = 10000;   // any failure
constexpr uint32_t kSameFailureGapMs = 30000;   // identical failure

class Policy {
public:
    // True when `e` deserves a notice now; fills `out`. Cancelled and
    // Recalculating are always silent (Rerouted reports the result).
    bool Accept(const feedback::Event& e, uint32_t now, Notice& out);
    void Reset() { *this = Policy(); }
private:
    uint32_t m_lastStart = 0, m_lastReroute = 0, m_lastArrived = 0, m_lastFailure = 0;
    bool     m_anyStart = false, m_anyReroute = false, m_anyArrived = false, m_anyFailure = false;
    feedback::EventType m_lastFailureType = feedback::EventType::Failed;
    RouteFailure        m_lastFailureReason = RouteFailure::None;
};

using Sink = void (*)(const Notice& notice, void* user);

void Install();                          // subscribe to gps_feedback (idempotent, GTA thread);
                                         // with SKR_GPS_C3_BRIDGE also installs the C3 sink
#if defined(SKR_GPS_C3_BRIDGE)
void InstallC3Sink();                    // gps_notify_c3.cpp (needs the C3 native_api_ui.h)
#endif
void SetSink(Sink sink, void* user);     // nullptr = count only
void Reset();                            // disconnect: rate-limit state

struct Stats { uint32_t accepted, delivered, suppressed; };
Stats GetStats();

} // namespace skr::gps::notify
