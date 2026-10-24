#include "gps_route.h"
#include "gps_flags.h"
#include "gps_graph.h"
#include <windows.h>
#include <chrono>
#include <cstring>
#include <algorithm>

namespace skr::gps {

// ── GTA addresses (SA 1.0 US) ──────────────────────────────────────

// Minimal CVector compatible with GTA's expected layout.
struct GtaVec3 { float x, y, z; };

// CNodeAddress: {short areaId, short nodeId} = 4 bytes
struct GtaNodeAddr {
    int16_t areaId = -1;
    int16_t nodeId = -1;
    bool IsEmpty() const { return areaId == -1 || nodeId == -1; }
};
static_assert(sizeof(GtaNodeAddr) == 4);

// CPathNode (0x1C bytes) — we only read positions
struct GtaPathNode {
    void*     ptr;             // 0x00
    void*     ptr2;            // 0x04
    int16_t   compX, compY, compZ; // 0x08 CompressedVector
    uint16_t  searchList;      // 0x0E
    int16_t   baseLinkId;      // 0x10
    int16_t   areaId;          // 0x12
    int16_t   nodeId;          // 0x14
    uint8_t   pathWidth;       // 0x16
    uint8_t   floodFill;       // 0x17
    uint8_t   flags1;          // 0x18
    uint8_t   flags2;          // 0x19
    uint8_t   flags3;          // 0x1A
    uint8_t   _pad;            // 0x1B
};
static_assert(sizeof(GtaPathNode) == 0x1C);

// CPathFind — opaque, we only need its address for __thiscall
struct CPathFindProxy {};

// ThePaths — CPathFind global instance at 0x96F050 (SA 1.0 US).
static CPathFindProxy* GetThePaths() {
    return reinterpret_cast<CPathFindProxy*>(0x96F050);
}

// ── Function pointers ───────────────────────────────────────────────

using FnDoPathSearch = void(__thiscall*)(
    CPathFindProxy* self,
    uint8_t   pathType,
    GtaVec3   origin,
    GtaNodeAddr originAddr,
    GtaVec3   target,
    GtaNodeAddr* pResultNodes,
    int16_t*  pNodesCount,
    int       maxNodesToFind,
    float*    pDistance,
    float     maxSearchDistance,
    GtaNodeAddr* targetAddr,
    float     maxUnkLimit,
    bool      oneSideOnly,
    GtaNodeAddr forbiddenNodeAddr,
    bool      includeNodesWithoutLinks,
    bool      waterPath
);
static auto DoPathSearch = reinterpret_cast<FnDoPathSearch>(0x4515D0);

using FnGetPathNode = GtaPathNode*(__thiscall*)(CPathFindProxy* self, GtaNodeAddr addr);
static auto GetPathNode = reinterpret_cast<FnGetPathNode>(0x420AC0);

using FnSetPathsNeeded = void(__thiscall*)(CPathFindProxy* self, const GtaVec3& pos);
static auto SetPathsNeededAtPosition = reinterpret_cast<FnSetPathsNeeded>(0x44DCD0);

// CPathNode::GetNodeCoors — 0x420A10, __thiscall, returns CVector by value
using FnGetNodeCoors = GtaVec3(__thiscall*)(GtaPathNode* self);
static auto GetNodeCoors = reinterpret_cast<FnGetNodeCoors>(0x420A10);

// ── Module state ────────────────────────────────────────────────────

static RouteSnapshot g_route;

// ── Helpers ─────────────────────────────────────────────────────────

static Vec3 NodeToVec3(CPathFindProxy* paths, const GtaNodeAddr& addr) {
    Vec3 result{};
    __try {
        GtaPathNode* node = GetPathNode(paths, addr);
        if (node) {
            GtaVec3 coors = GetNodeCoors(node);
            result = {coors.x, coors.y, coors.z};
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return result;
}

static float SegmentDistance2D(const Vec3& p, const Vec3& a, const Vec3& b) {
    const float abx = b.x - a.x, aby = b.y - a.y;
    const float ab2 = abx * abx + aby * aby;
    float t = ab2 > 0.01f ? ((p.x - a.x) * abx + (p.y - a.y) * aby) / ab2 : 0.0f;
    t = (std::max)(0.0f, (std::min)(1.0f, t));
    const float dx = p.x - (a.x + t * abx), dy = p.y - (a.y + t * aby);
    return std::sqrt(dx * dx + dy * dy);
}

static float ComputeRouteDistance(const std::vector<RouteNode>& nodes, int fromIndex) {
    float dist = 0.0f;
    for (int i = fromIndex; i + 1 < static_cast<int>(nodes.size()); ++i)
        dist += nodes[i].pos.DistanceTo(nodes[i + 1].pos);
    return dist;
}

// ── Public API ──────────────────────────────────────────────────────

// SEH-safe wrapper: calls DoPathSearch and GetPathNode, writes to plain arrays.
// Returns the number of resolved route positions, or -1 on exception.
struct PathSearchResult {
    Vec3    positions[kMaxRouteNodes];
    int16_t areaIds[kMaxRouteNodes];
    int16_t nodeIds[kMaxRouteNodes];
    int16_t count    = 0;
    float   distance = 0.0f;
    bool    ok       = false;
};

static int DoPathSearchSafe(const Vec3& origin, const Vec3& target, PathSearchResult& out) {
    out = PathSearchResult{};
    __try {
        auto* paths = GetThePaths();
        if (!paths) return -1;

        GtaVec3 gOrigin{origin.x, origin.y, origin.z};
        GtaVec3 gTarget{target.x, target.y, target.z};
        SetPathsNeededAtPosition(paths, gOrigin);
        SetPathsNeededAtPosition(paths, gTarget);

        GtaNodeAddr resultNodes[kMaxRouteNodes] = {};
        int16_t     nodesCount = 0;
        float       distance   = 0.0f;
        GtaNodeAddr emptyAddr{-1, -1};

        DoPathSearch(
            paths, 0, gOrigin, emptyAddr, gTarget,
            resultNodes, &nodesCount, kMaxRouteNodes,
            &distance, 999999.0f, nullptr, 750.0f,
            false, emptyAddr, true, false
        );

        if (nodesCount <= 0) return 0;

        int resolved = 0;
        for (int16_t i = 0; i < nodesCount && resolved < kMaxRouteNodes; ++i) {
            if (resultNodes[i].IsEmpty()) continue;
            Vec3 pos = NodeToVec3(paths, resultNodes[i]);
            if (pos.x == 0.0f && pos.y == 0.0f && pos.z == 0.0f) continue;
            out.positions[resolved] = pos;
            out.areaIds[resolved]   = resultNodes[i].areaId;
            out.nodeIds[resolved]   = resultNodes[i].nodeId;
            ++resolved;
        }
        out.count    = static_cast<int16_t>(resolved);
        out.distance = distance;
        out.ok       = true;
        return resolved;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

static int NativeSearchGta(const Vec3& origin, const Vec3& target, std::vector<RouteNode>& out) {
    out.clear();
    PathSearchResult psr;
    const int result = DoPathSearchSafe(origin, target, psr);
    if (result <= 0 || !psr.ok) return result;
    out.reserve(psr.count);
    for (int i = 0; i < psr.count; ++i)
        out.push_back({psr.positions[i], psr.areaIds[i], psr.nodeIds[i]});
    return static_cast<int>(out.size());
}

static NativeSearchFn g_nativeSearch = &NativeSearchGta;

void SetNativeSearchForTesting(NativeSearchFn fn) {
    g_nativeSearch = fn ? fn : &NativeSearchGta;
}

// DoPathSearch resolves endpoints to the nearest *loaded* node at any
// distance, so a far-off end node means the destination area was not loaded.
static constexpr float kNativeEndpointTolerance = 100.0f;

const char* ToString(RouteFailure f) {
    switch (f) {
        case RouteFailure::None:                  return "none";
        case RouteFailure::NoRoadNearOrigin:      return "no road near player";
        case RouteFailure::NoRoadNearDestination: return "no road near destination";
        case RouteFailure::Disconnected:          return "destination not reachable by road";
        case RouteFailure::NoPath:                return "no path";
        case RouteFailure::RoadDataUnavailable:   return "road data unavailable";
        case RouteFailure::RoadDataLoading:       return "road data loading";
        case RouteFailure::Timeout:               return "road data timeout";
    }
    return "?";
}

static RouteFailure FailureFor(graph::SearchStatus s) {
    switch (s) {
        case graph::SearchStatus::NoRoadNearOrigin:      return RouteFailure::NoRoadNearOrigin;
        case graph::SearchStatus::NoRoadNearDestination: return RouteFailure::NoRoadNearDestination;
        case graph::SearchStatus::Disconnected:          return RouteFailure::Disconnected;
        case graph::SearchStatus::Exhausted:             return RouteFailure::NoPath;
        default:                                         return RouteFailure::None;
    }
}

// Appends p unless it duplicates the last point (graph nodes and projections can coincide).
static void PushPoint(std::vector<RouteNode>& nodes, const Vec3& p, int16_t area, int16_t id) {
    if (!nodes.empty() && nodes.back().pos.DistanceTo(p) < 0.5f) return;
    nodes.push_back({p, area, id});
}

static RouteOutcome Finish(std::vector<RouteNode>&& nodes, RouteSource source, float distance) {
    g_route.nodes           = std::move(nodes);
    g_route.source          = source;
    g_route.failure         = RouteFailure::None;
    g_route.totalDistance   = distance > 0.0f ? distance : ComputeRouteDistance(g_route.nodes, 0);
    g_route.remainingDist   = g_route.totalDistance;
    g_route.currentIndex    = 0;
    g_route.roadDestination = g_route.nodes.back().pos;
    g_route.destinationSnap = g_route.roadDestination.DistanceTo2D(g_route.destination);
    g_route.status          = NavStatus::Navigating;
    return RouteOutcome::Found;
}

static RouteOutcome Fail(NavStatus status, RouteOutcome outcome, RouteFailure failure) {
    const Vec3 dest = g_route.destination;
    g_route = RouteSnapshot{};
    g_route.destination = dest;
    g_route.status = status;
    g_route.failure = failure;
    return outcome;
}

// Native routes end at GTA's road nodes. When the road graph is loaded, extend
// them to the exact on-road start/end points, but only along a link that
// touches the native route's end node (never a straight shortcut).
static void ExtendNativeEnds(std::vector<RouteNode>& nodes, const Vec3& origin, const Vec3& target) {
    const graph::RoadGraph* g = graph::Get();
    if (!g || nodes.empty()) return;
    auto touches = [&](const graph::RoadPoint& rp, const RouteNode& n) {
        const uint32_t id = g->Global(n.areaId, n.nodeId);
        return rp.Valid() && id != graph::kNoNode && (id == rp.a || id == rp.b);
    };
    const graph::RoadPoint end = graph::NearestRoadPoint(*g, target, 0.0f, 300.0f);
    if (touches(end, nodes.back())) PushPoint(nodes, end.point, -1, -1);
    const graph::RoadPoint start = graph::NearestRoadPoint(*g, origin, 2.0f, 300.0f);
    if (touches(start, nodes.front()) && nodes.front().pos.DistanceTo(start.point) >= 0.5f)
        nodes.insert(nodes.begin(), RouteNode{start.point, -1, -1});
}

RouteOutcome CalculateRoute(const Vec3& origin, const Vec3& target) {
    g_route = RouteSnapshot{};
    g_route.status      = NavStatus::Calculating;
    g_route.destination = target;
    const float straight = origin.DistanceTo2D(target);

    // 1. Local: native search, accepted only when complete and on target.
    if (straight <= kNativeRange) {
        std::vector<RouteNode> nodes;
        const int count = g_nativeSearch(origin, target, nodes);
        const char* reject = nullptr;
        if (count < 0)                                   reject = "fault";
        else if (count == 0)                             reject = "no nodes";
        else if (count >= kMaxRouteNodes)                reject = "buffer full (truncated)";
        else if (nodes.back().pos.DistanceTo2D(target) > kNativeEndpointTolerance)
                                                         reject = "ends away from destination";
        else if (nodes.front().pos.DistanceTo2D(origin) > kNativeEndpointTolerance)
                                                         reject = "starts away from player";
        if (!reject) {
            ExtendNativeEnds(nodes, origin, target);
            Log("route: native nodes=%d (+%zu on-road ends) straight=%.0f -> accepted",
                count, nodes.size() - static_cast<size_t>(count), straight);
            return Finish(std::move(nodes), RouteSource::Native, 0.0f);
        }
        Log("route: native nodes=%d straight=%.0f -> rejected (%s), using road graph", count, straight, reject);
    }

    // 2. Road graph (whole map).
    graph::RequestLoad();
    const graph::RoadGraph* g = graph::Get();
    if (!g) {
        if (graph::GetLoadState() == graph::LoadState::Failed) {
            Log("route: road graph unavailable: %s", graph::LoadError());
            return Fail(NavStatus::Failed, RouteOutcome::Failed, RouteFailure::RoadDataUnavailable);
        }
        return Fail(NavStatus::Calculating, RouteOutcome::Pending, RouteFailure::RoadDataLoading);
    }

    const auto t0 = std::chrono::steady_clock::now();
    const graph::SearchResult r = graph::FindRoute(*g, origin, target);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    Log("route: graph %s nodes=%zu len=%.0f straight=%.0f snap=%.0f/%.0f%s expanded=%u %.1fms",
        graph::ToString(r.status), r.path.size(), r.length, straight,
        r.originSnapDist, r.destSnapDist, r.destReSnapped ? " (nearest road unreachable, re-snapped)" : "",
        r.expanded, ms);
    if (r.status != graph::SearchStatus::Found)
        return Fail(NavStatus::Unreachable, RouteOutcome::Unreachable, FailureFor(r.status));

    std::vector<RouteNode> nodes;
    nodes.reserve(r.path.size() + 2);
    PushPoint(nodes, r.start, -1, -1);
    for (uint32_t id : r.path) {
        const graph::Node& n = g->nodes[id];
        PushPoint(nodes, {n.x, n.y, n.z}, n.area, n.id);
    }
    PushPoint(nodes, r.end, -1, -1);
    if (nodes.size() < 2) nodes.push_back(nodes.back());   // start == end: zero-length route, arrives at once
    return Finish(std::move(nodes), RouteSource::Graph, r.length);
}

void InvalidateRoute() {
    g_route = RouteSnapshot{};
}

void AbandonRoute(NavStatus status, RouteFailure failure) {
    Fail(status, RouteOutcome::Failed, failure);
}

const RouteSnapshot& GetRouteSnapshot() {
    return g_route;
}

NavStatus GetNavStatus() {
    return g_route.status;
}

bool UpdateNavigation(const Vec3& playerPos) {
    if (g_route.status != NavStatus::Navigating) return false;
    if (g_route.nodes.empty()) return false;

    // Advance past nodes the player has reached (within 20 units).
    constexpr float kNodeReachDist = 20.0f;
    bool crossed = false;

    // A skipped node (cut corner, start node behind the player) must not stall
    // progress on long routes: jump to the nearest upcoming segment the player
    // is on. The segment's end node becomes the next node to reach.
    constexpr int   kLookahead  = 40;
    constexpr float kOnSegment  = 25.0f;
    const int count = static_cast<int>(g_route.nodes.size());
    int bestSeg = -1;
    float bestDist = kOnSegment;
    for (int i = (std::max)(1, g_route.currentIndex + 1);
         i < (std::min)(count, g_route.currentIndex + kLookahead); ++i) {
        const float d = SegmentDistance2D(playerPos, g_route.nodes[i - 1].pos, g_route.nodes[i].pos);
        if (d < bestDist) { bestDist = d; bestSeg = i; }
    }
    if (bestSeg > g_route.currentIndex) {
        g_route.currentIndex = bestSeg;
        crossed = true;
    }

    while (g_route.currentIndex < static_cast<int>(g_route.nodes.size())) {
        float dist = playerPos.DistanceTo2D(g_route.nodes[g_route.currentIndex].pos);
        if (dist > kNodeReachDist) break;
        ++g_route.currentIndex;
        crossed = true;
    }

    // Update remaining distance.
    if (g_route.currentIndex < static_cast<int>(g_route.nodes.size())) {
        float distToNext = playerPos.DistanceTo2D(g_route.nodes[g_route.currentIndex].pos);
        g_route.remainingDist = distToNext + ComputeRouteDistance(g_route.nodes, g_route.currentIndex);
    } else {
        g_route.remainingDist = 0.0f;
    }

    return crossed;
}

bool HasDeviated(const Vec3& playerPos, float deviationThreshold) {
    if (g_route.status != NavStatus::Navigating) return false;
    if (g_route.nodes.empty()) return false;

    // Find the minimum distance from the player to any upcoming route segment.
    float minDist = 999999.0f;
    int start = (std::max)(0, g_route.currentIndex - 1);
    int end   = (std::min)(static_cast<int>(g_route.nodes.size()) - 1, g_route.currentIndex + 5);

    for (int i = start; i <= end; ++i) {
        float d = playerPos.DistanceTo2D(g_route.nodes[i].pos);
        if (d < minDist) minDist = d;
    }

    // Also check point-to-segment distance for the current segment.
    if (g_route.currentIndex < static_cast<int>(g_route.nodes.size())) {
        int prev = (std::max)(0, g_route.currentIndex - 1);
        const auto& a = g_route.nodes[prev].pos;
        const auto& b = g_route.nodes[g_route.currentIndex].pos;
        float abx = b.x - a.x, aby = b.y - a.y;
        float apx = playerPos.x - a.x, apy = playerPos.y - a.y;
        float ab2 = abx * abx + aby * aby;
        if (ab2 > 0.01f) {
            float t = (apx * abx + apy * aby) / ab2;
            t = (std::max)(0.0f, (std::min)(1.0f, t));
            float px = a.x + t * abx, py = a.y + t * aby;
            float dx = playerPos.x - px, dy = playerPos.y - py;
            float segDist = std::sqrt(dx * dx + dy * dy);
            if (segDist < minDist) minDist = segDist;
        }
    }

    return minDist > deviationThreshold;
}

} // namespace skr::gps
