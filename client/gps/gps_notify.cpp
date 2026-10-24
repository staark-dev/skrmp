#include "gps_notify.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>

namespace skr::gps::notify {
namespace {

Policy   g_policy;
Sink     g_sink = nullptr;
void*    g_sinkUser = nullptr;
int      g_subscription = 0;
Stats    g_stats{};

void FormatKm(char* out, size_t len, float metres) {
    if (metres < 1000.0f) snprintf(out, len, "%d m", static_cast<int>(metres / 10.0f + 0.5f) * 10);
    else                  snprintf(out, len, "%.1f km", metres / 1000.0f);
}

void OnEvent(const feedback::Event& e, void*) {
    Notice n;
    if (!g_policy.Accept(e, GetTickCount(), n)) { ++g_stats.suppressed; return; }
    ++g_stats.accepted;
    if (g_sink) {
        g_sink(n, g_sinkUser);
        ++g_stats.delivered;
    }
}

bool Since(bool any, uint32_t last, uint32_t now, uint32_t gap) { return !any || now - last >= gap; }

} // namespace

bool Policy::Accept(const feedback::Event& e, uint32_t now, Notice& out) {
    using T = feedback::EventType;
    out = Notice();
    char d[24];
    switch (e.type) {
        case T::RouteStarted:
            if (!Since(m_anyStart, m_lastStart, now, kStartCoalesceMs)) { m_lastStart = now; return false; }
            m_anyStart = true; m_lastStart = now;
            m_anyReroute = false;   // a new destination starts its own reroute budget
            m_anyFailure = false;
            FormatKm(d, sizeof(d), e.distance);
            snprintf(out.text, sizeof(out.text), "GPS route set \xC2\xB7 %s", d);
            return true;
        case T::Rerouted:
            if (!Since(m_anyReroute, m_lastReroute, now, kRerouteMinGapMs)) return false;
            m_anyReroute = true; m_lastReroute = now;
            out.durationSec = 3;
            snprintf(out.text, sizeof(out.text), "GPS route recalculated");
            return true;
        case T::Arrived:
            if (!Since(m_anyArrived, m_lastArrived, now, kArrivedDedupeMs)) return false;
            m_anyArrived = true; m_lastArrived = now;
            out.kind = Kind::Success;
            snprintf(out.text, sizeof(out.text), "You have arrived at your destination");
            return true;
        case T::Unreachable:
        case T::Failed: {
            const bool same = m_anyFailure && m_lastFailureType == e.type && m_lastFailureReason == e.failure;
            if (!Since(m_anyFailure, m_lastFailure, now, same ? kSameFailureGapMs : kFailureMinGapMs)) return false;
            m_anyFailure = true; m_lastFailure = now;
            m_lastFailureType = e.type; m_lastFailureReason = e.failure;
            out.kind = Kind::Error;
            out.durationSec = 5;
            snprintf(out.text, sizeof(out.text), "GPS: %s",
                     e.failure != RouteFailure::None ? ToString(e.failure)
                     : e.type == T::Unreachable ? "no road route to the waypoint" : "route unavailable");
            return true;
        }
        case T::Recalculating:
        case T::Cancelled:
        default:
            return false;
    }
}

void Install() {
    if (g_subscription) return;
    g_subscription = feedback::Subscribe(&OnEvent, nullptr);
#if defined(SKR_GPS_C3_BRIDGE)
    InstallC3Sink();
#endif
}

void SetSink(Sink sink, void* user) {
    g_sink = sink;
    g_sinkUser = user;
}

void Reset() { g_policy.Reset(); }

Stats GetStats() { return g_stats; }

} // namespace skr::gps::notify
