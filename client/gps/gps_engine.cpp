#include "gps_engine.h"
#include "gps_destination.h"
#include "gps_feedback.h"
#include "gps_flags.h"
#include "gps_graph.h"
#include "gps_renderer.h"
#include "gps_route.h"
#include "gps_zoom.h"
#include <windows.h>

namespace skr::gps {

// ── Timing ──────────────────────────────────────────────────────────
static constexpr DWORD kWaypointCheckMs   = 200;    // map waypoint 5×/s
static constexpr DWORD kDeviationCheckMs  = 500;    // deviation 2×/s
static constexpr DWORD kRecalcCooldownMs  = 2000;   // ≥ 2 s between off-route recalculations
static constexpr DWORD kRecalcWindowMs    = 60000;  // at most kMaxRecalcsPerWindow per 60 s
static constexpr int   kMaxRecalcsPerWindow = 10;
static constexpr DWORD kPendingRetryMs    = 250;    // road graph still loading
static constexpr DWORD kPendingTimeoutMs  = 20000;
static constexpr DWORD kOriginRetryMs     = 3000;   // player away from roads (interior, off-map)
static constexpr DWORD kSummaryMs         = 200;
static constexpr DWORD kSummaryLogMs      = 30000;
static constexpr float kRouteEndArrival   = 20.0f;  // at the on-road end of the route
static constexpr float kNearArrivalRoute  = 120.0f; // no off-route recalculation this close to the end
static constexpr float kOffRoadApproach   = 30.0f;  // slack beyond the off-road gap to the waypoint
static constexpr float kSameWaypoint      = 5.0f;

static DWORD g_lastWaypointCheck  = 0;
static DWORD g_lastDeviationCheck = 0;
static DWORD g_lastRecalcTime     = 0;
static DWORD g_lastPendingRetry   = 0;
static DWORD g_pendingSince       = 0;
static DWORD g_lastOriginRetry    = 0;
static DWORD g_lastSummary        = 0;
static DWORD g_lastSummaryLog     = 0;
static DWORD g_recalcWindowStart  = 0;
static int   g_recalcsInWindow    = 0;

static Vec3  g_lastWaypointPos = {};
static bool  g_lastWaypointValid = false;
static Vec3  g_suppressedPos = {};      // arrived waypoint GTA refused to clear
static bool  g_suppressed = false;
static bool  g_recalculating = false;   // off-route recalculation in flight
static bool  g_justArrived = false;     // summary shows Arrived until the next destination
static DWORD g_arrivedAt = 0;

static bool GtaClear() { return ClearMapWaypoint(); }
static const WaypointSource kGtaSource{&HasMapWaypoint, &GetMapWaypointPosition, &GtaClear};
static const WaypointSource* g_src = &kGtaSource;

void SetWaypointSourceForTesting(const WaypointSource* src) { g_src = src ? src : &kGtaSource; }

static bool Near2D(const Vec3& a, const Vec3& b, float d) {
    return std::abs(a.x - b.x) <= d && std::abs(a.y - b.y) <= d;
}

static int GraphLinkCount(int16_t area, int16_t node) {
    const graph::RoadGraph* g = graph::Get();
    if (!g || area < 0 || node < 0) return -1;
    const uint32_t id = g->Global(area, node);
    return id == graph::kNoNode ? -1 : g->nodes[id].linkCount;
}

static void EmitEvent(feedback::EventType type, float distance = 0.0f) {
    feedback::Event e;
    e.type = type;
    e.failure = GetRouteSnapshot().failure;
    e.distance = distance;
    e.tick = GetTickCount();
    Log("event: %s failure=%s dist=%.0f", feedback::ToString(type), ToString(e.failure), distance);
    feedback::Emit(e);
}

// Calculates and reports. `rerouting` = off-route recalculation (not a new destination).
static void Calculate(const Vec3& from, const Vec3& to, DWORD now, const char* why, bool rerouting) {
    const RouteOutcome o = CalculateRoute(from, to);
    const RouteSnapshot& r = GetRouteSnapshot();
    if (o == RouteOutcome::Pending) {
        const bool first = g_pendingSince == 0;
        if (first) g_pendingSince = now ? now : 1;
        if (!first) return;   // still loading: log only the first pending result
        Log("core: %s %.0f,%.0f -> pending (%s)", why, to.x, to.y, ToString(r.failure));
        return;
    }
    g_pendingSince = 0;
    g_recalculating = false;
    Log("core: %s %.0f,%.0f -> status=%d src=%d nodes=%zu dist=%.0f gap=%.0f failure=%s", why, to.x, to.y,
        static_cast<int>(r.status), static_cast<int>(r.source), r.nodes.size(), r.remainingDist,
        r.destinationSnap, ToString(r.failure));
    switch (o) {
        case RouteOutcome::Found:
            EmitEvent(rerouting ? feedback::EventType::Rerouted : feedback::EventType::RouteStarted, r.totalDistance);
            break;
        case RouteOutcome::Unreachable: EmitEvent(feedback::EventType::Unreachable); break;
        case RouteOutcome::Failed:      EmitEvent(feedback::EventType::Failed); break;
        default: break;
    }
}

static void OnArrival(const Vec3& playerPos, DWORD now) {
    const bool same = g_src->has() && Near2D(g_src->pos(), g_lastWaypointPos, kSameWaypoint);
    bool cleared = false;
    if (same) {
        cleared = g_src->clear();
        if (!cleared) { g_suppressed = true; g_suppressedPos = g_lastWaypointPos; }
    }
    Log("core: arrived at %.0f,%.0f (player %.0f,%.0f) -> waypoint %s, Idle",
        g_lastWaypointPos.x, g_lastWaypointPos.y, playerPos.x, playerPos.y,
        !same ? "already replaced/removed, not touched" : cleared ? "cleared" : "clear refused, suppressed");
    EmitEvent(feedback::EventType::Arrived);
    ClearDestination();
    InvalidateRoute();
    g_lastWaypointValid = false;
    g_lastWaypointPos   = {};
    g_pendingSince      = 0;
    g_recalculating     = false;
    g_justArrived       = true;
    g_arrivedAt         = now;
}

// ── Waypoint check (shared by Tick and SyncWaypoint) ────────────────

static void CheckWaypoint(const Vec3& playerPos, DWORD now) {
    if (now - g_lastWaypointCheck < kWaypointCheckMs) return;
    g_lastWaypointCheck = now;

    const bool hasWp = g_src->has();
    const Vec3 wpPos = hasWp ? g_src->pos() : Vec3{};
    if (g_suppressed && (!hasWp || !Near2D(wpPos, g_suppressedPos, kSameWaypoint)))
        g_suppressed = false;
    if (hasWp && !g_suppressed) {
        const bool changed = !g_lastWaypointValid ||
                             std::abs(wpPos.x - g_lastWaypointPos.x) > kSameWaypoint ||
                             std::abs(wpPos.y - g_lastWaypointPos.y) > kSameWaypoint;
        if (changed) {
            if (g_lastWaypointValid) Log("core: destination replaced while navigating");
            g_lastWaypointPos   = wpPos;
            g_lastWaypointValid = true;
            g_pendingSince      = 0;
            g_recalcsInWindow   = 0;
            g_recalcWindowStart = now;
            g_justArrived       = false;
            SetDestination(wpPos);
            Calculate(playerPos, wpPos, now, "waypoint", false);
            g_lastRecalcTime = now;
        }
    } else if (!hasWp && g_lastWaypointValid) {
        g_lastWaypointValid = false;
        g_lastWaypointPos   = {};
        g_pendingSince      = 0;
        g_recalculating     = false;
        ClearDestination();
        InvalidateRoute();
        Log("core: waypoint removed -> route invalidated");
        EmitEvent(feedback::EventType::Cancelled);
    }
}

static void RetryPending(const Vec3& playerPos, DWORD now) {
    if (GetNavStatus() == NavStatus::Calculating && HasDestination() && g_pendingSince != 0 &&
        now - g_lastPendingRetry >= kPendingRetryMs) {
        g_lastPendingRetry = now;
        Calculate(playerPos, GetDestination(), now, "retry", g_recalculating);
    }
}

static void RefreshSummary(const Vec3& playerPos, DWORD now) {
    if (now - g_lastSummary < kSummaryMs) return;
    g_lastSummary = now;
    const RouteSnapshot& r = GetRouteSnapshot();
    const feedback::Summary previous = feedback::GetSummary();
    feedback::Summary s;
    s.failure = r.failure;
    switch (r.status) {
        case NavStatus::Navigating:
            s.state = g_recalculating ? feedback::Summary::State::Recalculating : feedback::Summary::State::Navigating;
            s.remaining = r.remainingDist;
            s.next = feedback::NextManeuver(r.nodes, r.currentIndex, playerPos, &GraphLinkCount);
            break;
        case NavStatus::Calculating:
            s.state = g_recalculating ? feedback::Summary::State::Recalculating : feedback::Summary::State::Calculating;
            break;
        case NavStatus::Unreachable:
        case NavStatus::Failed:
            s.state = feedback::Summary::State::Unavailable;
            break;
        default:
            s.state = g_justArrived && now - g_arrivedAt < 5000 ? feedback::Summary::State::Arrived
                                                                : feedback::Summary::State::Idle;
            break;
    }
    s.stateSince = s.state == previous.state ? previous.stateSince : now;
    feedback::SetSummary(s);
    zoom::SetNavigationContext(s.state == feedback::Summary::State::Navigating,
                               s.next.reliable && s.next.maneuver != feedback::Maneuver::Arrive &&
                               s.next.maneuver != feedback::Maneuver::None ? s.next.distance : -1.0f);

    const RenderStats rs = GetRenderStats();
    static uint32_t s_reportedFaults = 0;
    if (rs.failures + rs.transformFaults > s_reportedFaults) {
        s_reportedFaults = rs.failures + rs.transformFaults;
        Log("render: draw failures=%u transform faults=%u (frames drawn=%u)", rs.failures, rs.transformFaults, rs.frames);
    }
    if (s.state != feedback::Summary::State::Idle && now - g_lastSummaryLog >= kSummaryLogMs) {
        g_lastSummaryLog = now;
        Log("summary: %s remaining=%.0f idx=%d/%zu next=%s@%.0f%s recalcs=%d/60s render frames=%u pieces=%u",
            feedback::ToString(s.state), s.remaining, r.currentIndex, r.nodes.size(),
            feedback::ToString(s.next.maneuver), s.next.distance, s.next.reliable ? "" : "(unreliable)",
            g_recalcsInWindow, rs.frames, rs.pieces);
    }
}

void SyncWaypoint(float playerX, float playerY, float playerZ) {
    if (!flags::CoreEnabledCached()) return;   // called from ESC MAP rendering
    graph::RequestLoad();
    const Vec3 playerPos{playerX, playerY, playerZ};
    const DWORD now = GetTickCount();
    CheckWaypoint(playerPos, now);
    RetryPending(playerPos, now);
    RefreshSummary(playerPos, now);
}

// ── Tick ────────────────────────────────────────────────────────────

void Tick(float playerX, float playerY, float playerZ) {
    if (!flags::CoreEnabled()) {
        zoom::SetNavigationContext(false, -1.0f);
        return;
    }
    graph::RequestLoad();   // one-time background load; ready before most first waypoints
    if (flags::ZoomEnabled()) zoom::Install();   // GTA code stays unpatched unless enabled
    const Vec3 playerPos{playerX, playerY, playerZ};
    const DWORD now = GetTickCount();

    CheckWaypoint(playerPos, now);
    const NavStatus status = GetNavStatus();

    if (status == NavStatus::Calculating && HasDestination() && g_pendingSince != 0) {
        if (now - g_pendingSince >= kPendingTimeoutMs) {
            g_pendingSince = 0;
            g_recalculating = false;
            AbandonRoute(NavStatus::Failed, RouteFailure::Timeout);
            Log("core: road graph not ready after %lus (%s) -> Failed", kPendingTimeoutMs / 1000,
                graph::GetLoadState() == graph::LoadState::Failed ? graph::LoadError() : "still loading");
            EmitEvent(feedback::EventType::Failed);
        } else {
            RetryPending(playerPos, now);
        }
    } else if (status == NavStatus::Unreachable && HasDestination() &&
               GetRouteSnapshot().failure == RouteFailure::NoRoadNearOrigin &&
               now - g_lastOriginRetry >= kOriginRetryMs) {
        // Origin-dependent failure (interior, off-map): retry as the player moves.
        g_lastOriginRetry = now;
        const RouteOutcome o = CalculateRoute(playerPos, GetDestination());
        if (o == RouteOutcome::Found) {
            Log("core: player back near a road -> route found");
            EmitEvent(feedback::EventType::RouteStarted, GetRouteSnapshot().totalDistance);
        }
    } else if (status == NavStatus::Navigating) {
        UpdateNavigation(playerPos);

        // Arrival: within the waypoint radius, or at the route's on-road end.
        const RouteSnapshot& route = GetRouteSnapshot();
        if (IsArrived(playerPos) ||
            (!route.nodes.empty() && playerPos.DistanceTo2D(route.nodes.back().pos) <= kRouteEndArrival)) {
            OnArrival(playerPos, now);
            RefreshSummary(playerPos, now);
            return;
        }

        if (now - g_lastDeviationCheck >= kDeviationCheckMs) {
            g_lastDeviationCheck = now;
            if (now - g_recalcWindowStart >= kRecalcWindowMs) {
                g_recalcWindowStart = now;
                g_recalcsInWindow = 0;
            }
            // Leaving the road to reach an off-road waypoint is not a deviation.
            const bool nearArrival =
                route.remainingDist <= kNearArrivalRoute ||
                playerPos.DistanceTo2D(route.destination) <= route.destinationSnap + kOffRoadApproach;
            if (!nearArrival && HasDeviated(playerPos) && now - g_lastRecalcTime >= kRecalcCooldownMs) {
                if (g_recalcsInWindow < kMaxRecalcsPerWindow) {
                    ++g_recalcsInWindow;
                    g_lastRecalcTime = now;
                    g_recalculating = true;
                    EmitEvent(feedback::EventType::Recalculating, route.remainingDist);
                    Calculate(playerPos, GetDestination(), now, "off-route recalculation", true);
                } else if (now - g_lastRecalcTime >= kRecalcWindowMs / 2) {
                    g_lastRecalcTime = now;
                    Log("core: off-route but %d recalculations in the last 60 s; waiting", g_recalcsInWindow);
                }
            }
        }
    }
    RefreshSummary(playerPos, now);
}

void Reset() {
    ClearDestination();
    InvalidateRoute();
    g_lastWaypointCheck  = 0;
    g_lastDeviationCheck = 0;
    g_lastRecalcTime     = 0;
    g_lastPendingRetry   = 0;
    g_pendingSince       = 0;
    g_lastOriginRetry    = 0;
    g_lastSummary        = 0;
    g_lastSummaryLog     = 0;
    g_recalcWindowStart  = 0;
    g_recalcsInWindow    = 0;
    g_lastWaypointPos    = {};
    g_lastWaypointValid  = false;
    g_suppressed         = false;
    g_suppressedPos      = {};
    g_recalculating      = false;
    g_justArrived        = false;
    feedback::SetSummary({});
    zoom::SetNavigationContext(false, -1.0f);
}

void Shutdown() {
    Reset();
    zoom::Uninstall();
    ResetRenderer();
}

const RouteSnapshot& GetRoute() {
    return GetRouteSnapshot();
}

NavStatus GetStatus() {
    return GetNavStatus();
}

} // namespace skr::gps
