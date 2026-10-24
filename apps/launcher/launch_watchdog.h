#pragma once
// Launch handoff watchdog policy (pure, unit-tested in tests/unit/test_handoff.cpp).
//
// Armed when a launch starts. It protects against launches that never reach
// a usable session (stuck loading, crashed renderer, broken handoff). It is
// disarmed — permanently for that launch — as soon as the client reports
// either visual-ready (cover dismissed after a presented frame) or gameplay
// confirmed (in world and healthy). A confirmed session is never terminated
// by the watchdog, even if visual-ready never arrives.

#include <cstdint>

namespace skr::launcher {

constexpr uint64_t kHandoffTimeoutMs = 120000;

enum class WatchdogAction : uint8_t { None, DisarmVisualReady, DisarmGameplay, Terminate };

struct WatchdogInput {
    bool     armed = false;
    bool     visualReady = false;
    bool     gameplayConfirmed = false;
    uint64_t now = 0;
    uint64_t deadline = 0;
};

inline WatchdogAction DecideWatchdog(const WatchdogInput& in) {
    if (!in.armed) return WatchdogAction::None;
    if (in.visualReady) return WatchdogAction::DisarmVisualReady;
    if (in.gameplayConfirmed) return WatchdogAction::DisarmGameplay;
    if (in.now >= in.deadline) return WatchdogAction::Terminate;
    return WatchdogAction::None;
}

} // namespace skr::launcher
