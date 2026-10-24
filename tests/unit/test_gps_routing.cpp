// API-003.0.4 — long-distance GPS routing (real GTA road data + synthetic graphs)
// and destination arrival cleanup. Links the real client/gps sources.
// Real-data checks need gta3.img: SKR_GTA_IMG env var, else the Steam default.
// Exit 0 = pass, 1 = fail.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "gps/gps_graph.h"
#include "gps/gps_route.h"
#include "gps/gps_engine.h"
#include "gps/gps_destination.h"
#include "gps/gps_flags.h"

using namespace skr::gps;
namespace G = skr::gps::graph;

static int g_pass = 0, g_fail = 0, g_skip = 0;
#define CHECK(cond, msg) do { if (cond) { ++g_pass; } else { ++g_fail; std::printf("  FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

// ── Synthetic graph builder ─────────────────────────────────────────
// All synthetic nodes live in area 0 (x,y in [-3000,-2250]) unless placed.
struct Syn {
    std::vector<G::AreaFile> areas = std::vector<G::AreaFile>(G::kAreas);
    std::vector<std::vector<int>> adj;   // per node in area 0
    int Add(float x, float y, uint8_t flood = 1, bool vehicle = true, bool water = false) {
        G::Node n; n.x = x; n.y = y; n.z = 0; n.area = 0; n.id = (int16_t)areas[0].nodes.size();
        n.floodFill = flood; n.vehicle = vehicle; n.water = water;
        areas[0].nodes.push_back(n); adj.emplace_back();
        return n.id;
    }
    void Link(int a, int b) { adj[a].push_back(b); adj[b].push_back(a); }
    G::RoadGraph Build() {
        for (size_t i = 0; i < adj.size(); ++i) {
            G::Node& n = areas[0].nodes[i];
            n.firstLink = (uint32_t)areas[0].links.size();
            n.linkCount = (uint8_t)adj[i].size();
            for (int t : adj[i]) areas[0].links.push_back({0, (uint16_t)t});
        }
        G::RoadGraph g; std::string err;
        bool ok = G::BuildGraph(areas, g, err);
        if (!ok) std::printf("  BuildGraph: %s\n", err.c_str());
        return g;
    }
};

static bool Linked(const G::RoadGraph& g, uint32_t a, uint32_t b) {
    const G::Node& n = g.nodes[a];
    for (uint32_t k = 0; k < n.linkCount; ++k) if (g.links[n.firstLink + k] == b) return true;
    return false;
}

static bool PathConnected(const G::RoadGraph& g, const std::vector<uint32_t>& p) {
    for (size_t i = 1; i < p.size(); ++i) if (!Linked(g, p[i - 1], p[i])) return false;
    return true;
}

static void TestSyntheticSearch() {
    std::puts("== Synthetic A* ==");
    Syn s;
    // Ladder: 0-1-2-3-4 along x, detour 1-5-3 longer.
    int n0 = s.Add(-2990, -2990), n1 = s.Add(-2900, -2990), n2 = s.Add(-2800, -2990),
        n3 = s.Add(-2700, -2990), n4 = s.Add(-2600, -2990), n5 = s.Add(-2800, -2800);
    s.Link(n0, n1); s.Link(n1, n2); s.Link(n2, n3); s.Link(n3, n4); s.Link(n1, n5); s.Link(n5, n3);
    // Island (different flood fill) and a water-only bridge.
    int i0 = s.Add(-2400, -2400, 2), i1 = s.Add(-2350, -2400, 2); s.Link(i0, i1);
    int w = s.Add(-2650, -2800, 1, true, true); s.Link(n4, w);
    int ped = s.Add(-2600, -2700, 1, false); s.Link(n4, ped);
    G::RoadGraph g = s.Build();

    auto r = G::FindRoute(g, {-2990, -2990, 0}, {-2600, -2990, 0});
    CHECK(r.status == G::SearchStatus::Found, "ladder found");
    bool usesDetour = false;
    for (uint32_t id : r.path) usesDetour |= id == (uint32_t)n5;
    CHECK(!usesDetour && r.path.size() >= 3, "shortest path avoids detour");
    CHECK(PathConnected(g, r.path), "every step is a real link");
    CHECK(std::fabs(r.start.x + 2990.0f) < 0.01f && std::fabs(r.end.x + 2600.0f) < 0.01f,
          "route starts/ends at the on-road points");
    CHECK(std::fabs(r.length - 390.0f) < 0.5f, "length = sum of segments");

    r = G::FindRoute(g, {-2990, -2990, 0}, {-2400, -2400, 0});
    CHECK(r.status == G::SearchStatus::Disconnected && r.path.empty(), "other flood fill: disconnected, no path");

    r = G::FindRoute(g, {-2990, -2990, 0}, {-2250, -2990, 0}, {300.0f, 100.0f});
    CHECK(r.status == G::SearchStatus::NoRoadNearDestination, "destination beyond snap radius");

    r = G::FindRoute(g, {-2000, -2000, 0}, {-2600, -2990, 0}, {100.0f, 300.0f});
    CHECK(r.status == G::SearchStatus::NoRoadNearOrigin, "origin beyond snap radius");

    float d = 0;
    CHECK(G::NearestNode(g, {-2650, -2800, 0}, false, 30.0f, &d) == G::kNoNode, "water node is never an endpoint");
    CHECK(G::NearestNode(g, {-2600, -2700, 0}, false, 30.0f, &d) == G::kNoNode, "ped node is never an endpoint");

    // Same flood fill but the only connection runs through a water node.
    Syn t;
    int a = t.Add(-2990, -2990), b = t.Add(-2900, -2990, 1, true, true), c = t.Add(-2800, -2990);
    t.Link(a, b); t.Link(b, c);
    G::RoadGraph tg = t.Build();
    r = G::FindRoute(tg, {-2990, -2990, 0}, {-2800, -2990, 0});
    CHECK(r.status != G::SearchStatus::Found && r.path.empty(), "water-only connection: no path, no straight line");

    std::string err; G::AreaFile af;
    uint8_t junk[16] = {};
    CHECK(!G::ParseArea(0, junk, sizeof(junk), af, err), "short file rejected");
}

// ── Real GTA data ───────────────────────────────────────────────────
struct Place { const char* name; Vec3 p; };
static const Place kLS = {"Los Santos (Grove St)",  {2495.0f, -1665.0f, 13.3f}};
static const Place kSF = {"San Fierro (Doherty)",   {-2027.0f, 165.0f, 28.8f}};
static const Place kLV = {"Las Venturas (Strip)",   {2040.0f, 1520.0f, 10.7f}};

static G::RoadGraph g_real;
static bool g_realOk = false;

static void TestRealData() {
    std::puts("== Real GTA road data (gta3.img) ==");
    std::wstring img = L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Grand Theft Auto San Andreas\\models\\gta3.img";
    if (const wchar_t* env = _wgetenv(L"SKR_GTA_IMG")) img = env;
    if (GetFileAttributesW(img.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::puts("  SKIPPED: gta3.img not found (set SKR_GTA_IMG)");
        ++g_skip;
        return;
    }
    auto t0 = std::chrono::steady_clock::now();
    std::string err;
    g_realOk = G::LoadFromImg(img, g_real, err);
    auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (!g_realOk) std::printf("  load error: %s\n", err.c_str());
    CHECK(g_realOk, "all 64 nodes*.dat parsed and validated");
    if (!g_realOk) return;
    std::printf("  loaded %zu nodes (%u vehicle), %zu links in %.0f ms\n",
                g_real.nodes.size(), g_real.vehicleNodes, g_real.links.size(), ms);
    CHECK(g_real.vehicleNodes == 30587, "vehicle node total matches nodes*.dat headers");

    size_t unresolved = 0, floodMismatch = 0, vehToPed = 0;
    for (uint32_t i = 0; i < g_real.nodes.size(); ++i) {
        const G::Node& n = g_real.nodes[i];
        for (uint32_t k = 0; k < n.linkCount; ++k) {
            const uint32_t t = g_real.links[n.firstLink + k];
            if (t == G::kNoNode) { ++unresolved; continue; }
            if (n.vehicle && !g_real.nodes[t].vehicle) ++vehToPed;
            if (g_real.Routable(i) && g_real.Routable(t) && g_real.nodes[t].floodFill != n.floodFill) ++floodMismatch;
        }
    }
    std::printf("  unresolved links=%zu, vehicle->ped links=%zu, flood-fill mismatches=%zu\n",
                unresolved, vehToPed, floodMismatch);
    CHECK(unresolved == 0, "every link resolves to a loaded node");
    CHECK(floodMismatch == 0, "linked routable nodes share flood fill (pre-check is sound)");

    const Place* pairs[][2] = {{&kLV, &kLS}, {&kLS, &kLV}, {&kLV, &kSF}, {&kSF, &kLV}, {&kLS, &kSF}, {&kSF, &kLS}};
    for (auto& pr : pairs) {
        const Vec3 a = pr[0]->p, b = pr[1]->p;
        auto t1 = std::chrono::steady_clock::now();
        G::SearchResult r = G::FindRoute(g_real, a, {b.x, b.y, 0.0f});
        double sms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
        const float straight = std::hypot(a.x - b.x, a.y - b.y);
        std::printf("  %-24s -> %-24s %s nodes=%zu len=%.0f straight=%.0f snap=%.0f/%.0f expanded=%u %.1f ms\n",
                    pr[0]->name, pr[1]->name, G::ToString(r.status), r.path.size(), r.length, straight,
                    r.originSnapDist, r.destSnapDist, r.expanded, sms);
        char m[160];
        snprintf(m, sizeof(m), "%s -> %s: found, connected, >64 nodes, sane length", pr[0]->name, pr[1]->name);
        CHECK(r.status == G::SearchStatus::Found && PathConnected(g_real, r.path) && r.path.size() > 64 &&
              r.length >= straight * 0.98f && r.length <= straight * 3.0f &&
              r.originSnapDist < 60.0f && r.destSnapDist < 60.0f, m);
        CHECK(sms < 250.0, "intercity search under 250 ms");
    }

    // Local: Grove Street to Ganton gym — short route stays short.
    G::SearchResult loc = G::FindRoute(g_real, kLS.p, {2229.0f, -1721.0f, 0.0f});
    std::printf("  local LS route: %s nodes=%zu len=%.0f\n", G::ToString(loc.status), loc.path.size(), loc.length);
    CHECK(loc.status == G::SearchStatus::Found && PathConnected(g_real, loc.path) && loc.length < 800.0f, "local route");

    // Open ocean far from any road.
    G::SearchResult sea = G::FindRoute(g_real, kLS.p, {-2900.0f, -2900.0f, 0.0f});
    CHECK(sea.status == G::SearchStatus::NoRoadNearDestination && sea.path.empty(), "ocean waypoint: unreachable, no path");
}

// ── Route planner integration (native search faked) ────────────────
static int g_nativeCount = 0;
static int g_nativeCalls = 0;
static int FakeNative(const Vec3& o, const Vec3& t, std::vector<RouteNode>& out) {
    ++g_nativeCalls;
    out.clear();
    for (int i = 0; i < g_nativeCount; ++i) {
        float f = g_nativeCount > 1 ? float(i) / float(g_nativeCount - 1) : 0.0f;
        out.push_back({{o.x + (t.x - o.x) * f, o.y + (t.y - o.y) * f, 0.0f}, 0, (int16_t)i});
    }
    return g_nativeCount;
}

static void TestPlanner() {
    std::puts("== Route planner ==");
    SetNativeSearchForTesting(&FakeNative);

    // Graph not loaded: long route is pending, never a partial success.
    G::SetForTesting(nullptr);
    g_nativeCount = 0;
    RouteOutcome o = CalculateRoute(kLV.p, kLS.p);
    CHECK(o == RouteOutcome::Pending && GetNavStatus() == NavStatus::Calculating, "graph not ready -> Calculating");
    CHECK(GetRouteSnapshot().nodes.empty(), "pending route has no nodes");

    if (!g_realOk) { std::puts("  SKIPPED: real-data planner checks"); ++g_skip; SetNativeSearchForTesting(nullptr); return; }
    G::SetForTesting(&g_real);

    // Local, native succeeds cleanly: native result is kept as-is.
    g_nativeCount = 20; g_nativeCalls = 0;
    const Vec3 localDest = {2229.0f, -1721.0f, 0.0f};
    o = CalculateRoute(kLS.p, localDest);
    CHECK(o == RouteOutcome::Found && GetRouteSnapshot().nodes.size() == 20 && g_nativeCalls == 1,
          "local: validated native route preserved");
    CHECK(GetRouteSnapshot().source == RouteSource::Native, "local source = native");

    // Local, native truncated at the 64 buffer: rejected, graph used.
    g_nativeCount = kMaxRouteNodes;
    o = CalculateRoute(kLS.p, localDest);
    CHECK(o == RouteOutcome::Found && GetRouteSnapshot().source == RouteSource::Graph, "truncated native -> graph");

    // Intercity: graph, complete, >64 nodes, ends at the destination road.
    g_nativeCount = 0; g_nativeCalls = 0;
    o = CalculateRoute(kLV.p, {kSF.p.x, kSF.p.y, 0.0f});
    const RouteSnapshot& s = GetRouteSnapshot();
    CHECK(o == RouteOutcome::Found && s.status == NavStatus::Navigating && s.source == RouteSource::Graph,
          "LV->SF navigating via graph");
    CHECK(s.nodes.size() > 64 && g_nativeCalls == 0, "no 64-node cap; native not attempted for intercity");
    CHECK(s.nodes.back().pos.DistanceTo2D(kSF.p) < 60.0f, "route ends at the destination");
    CHECK(std::fabs(s.totalDistance - s.remainingDist) < 0.01f && s.currentIndex == 0, "fresh route state");

    // Unreachable is reported, nothing drawn.
    o = CalculateRoute(kLS.p, {-2900.0f, -2900.0f, 0.0f});
    CHECK(o == RouteOutcome::Unreachable && GetNavStatus() == NavStatus::Unreachable &&
          GetRouteSnapshot().nodes.empty(), "ocean -> Unreachable, empty route");

    // Progress along a long route, driven segment by segment.
    o = CalculateRoute(kLV.p, {kLS.p.x, kLS.p.y, 0.0f});
    const RouteSnapshot& lr = GetRouteSnapshot();
    CHECK(o == RouteOutcome::Found && lr.nodes.size() > 64, "LV->LS found");
    if (o != RouteOutcome::Found) { SetNativeSearchForTesting(nullptr); return; }
    const float before = lr.remainingDist;
    const size_t half = lr.nodes.size() / 2;
    bool neverDeviated = true;
    for (size_t i = 1; i <= half; ++i) {
        const Vec3 a = lr.nodes[i - 1].pos, b = lr.nodes[i].pos;
        const Vec3 m = {(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, (a.z + b.z) * 0.5f};
        UpdateNavigation(m);
        neverDeviated = neverDeviated && !HasDeviated(m);
    }
    CHECK(lr.currentIndex >= (int)half && lr.remainingDist < before * 0.6f, "progress advances on a long route");
    CHECK(neverDeviated, "driving the route never counts as a deviation");
    const Vec3 mid = lr.nodes[half].pos;
    CHECK(HasDeviated({mid.x + 200.0f, mid.y + 200.0f, mid.z}), "200 units off-route is a deviation");

    // Cut corner: player skips node k entirely, 10 units beside segment k..k+1.
    CalculateRoute(kLV.p, {kLS.p.x, kLS.p.y, 0.0f});
    const size_t k = 5;
    const Vec3 a = lr.nodes[k].pos, b = lr.nodes[k + 1].pos;
    const float len = std::hypot(b.x - a.x, b.y - a.y);
    const Vec3 beside = {(a.x + b.x) * 0.5f - (b.y - a.y) / len * 10.0f,
                         (a.y + b.y) * 0.5f + (b.x - a.x) / len * 10.0f, a.z};
    UpdateNavigation(beside);
    CHECK(lr.currentIndex >= (int)k + 1, "skipped node does not stall progress");

    SetNativeSearchForTesting(nullptr);
}

// ── Engine: arrival cleanup ─────────────────────────────────────────
static bool g_wpActive = false;
static Vec3 g_wpPos = {};
static int  g_wpClears = 0;
static bool FakeHas() { return g_wpActive; }
static Vec3 FakePos() { return g_wpPos; }
static bool g_refuseClear = false;
static bool FakeClear() { ++g_wpClears; if (g_refuseClear) return false; g_wpActive = false; return true; }

static void TickFor(const Vec3& p, int ms) {
    for (int t = 0; t < ms; t += 50) { Tick(p.x, p.y, p.z); Sleep(50); }
}

static void TestArrival() {
    std::puts("== Engine: arrival cleanup ==");
    if (!g_realOk) { std::puts("  SKIPPED: needs real road data"); ++g_skip; return; }
    G::SetForTesting(&g_real);
    SetNativeSearchForTesting(&FakeNative);
    g_nativeCount = 0;
    flags::SetForTesting(flags::Flags{true, false, false});
    static const WaypointSource src{&FakeHas, &FakePos, &FakeClear};
    SetWaypointSourceForTesting(&src);
    Reset();

    const Vec3 start = kLS.p, dest = {2229.0f, -1721.0f, 0.0f};
    g_wpActive = true; g_wpPos = dest; g_wpClears = 0;
    TickFor(start, 300);
    CHECK(GetStatus() == NavStatus::Navigating, "waypoint -> Navigating");

    // Unreachable never clears the waypoint.
    g_wpPos = {-2900.0f, -2900.0f, 0.0f};
    TickFor(start, 300);
    CHECK(GetStatus() == NavStatus::Unreachable && g_wpActive && g_wpClears == 0, "pathfinding failure keeps the waypoint");

    // Back to a reachable destination, then arrive.
    g_wpPos = dest;
    TickFor(start, 300);
    CHECK(GetStatus() == NavStatus::Navigating, "new destination after failure");
    const Vec3 arrive = GetRoute().nodes.back().pos;
    TickFor(arrive, 300);
    CHECK(g_wpClears == 1 && !g_wpActive, "arrival clears the navigated waypoint once");
    CHECK(GetStatus() == NavStatus::Idle && !HasDestination(), "arrival -> Idle, no destination");
    TickFor(arrive, 300);
    CHECK(g_wpClears == 1 && GetStatus() == NavStatus::Idle, "stays Idle, no repeated clear");

    // A new waypoint at the same place is a fresh destination.
    g_wpActive = true; g_wpPos = dest;
    TickFor(start, 300);
    CHECK(GetStatus() == NavStatus::Navigating, "same-position waypoint after arrival navigates again");

    // A different waypoint placed while arriving is not cleared.
    const Vec3 other = {2495.0f, -1500.0f, 0.0f};
    g_wpPos = other;
    TickFor(GetRoute().nodes.empty() ? start : GetRoute().nodes.back().pos, 50);
    CHECK(g_wpActive, "a different waypoint is never cleared by arrival");

    // Manual removal invalidates immediately.
    g_wpActive = false;
    TickFor(start, 300);
    CHECK(GetStatus() == NavStatus::Idle && g_wpClears == 1, "manual removal -> Idle without native clear");

    // GTA refuses the clear: suppressed, no navigate/arrive loop.
    g_refuseClear = true;
    g_wpActive = true; g_wpPos = dest;
    TickFor(start, 300);
    CHECK(GetStatus() == NavStatus::Navigating, "navigating before refused clear");
    TickFor(GetRoute().nodes.back().pos, 1000);
    CHECK(g_wpClears == 2 && g_wpActive && GetStatus() == NavStatus::Idle, "refused clear: one attempt, Idle, no loop");
    g_refuseClear = false;
    g_wpPos = other;
    TickFor(start, 300);
    CHECK(GetStatus() == NavStatus::Navigating, "moving the suppressed waypoint navigates again");

    // ESC MAP path: SyncWaypoint alone (no Tick, game paused) builds and clears the route.
    Reset();
    g_wpActive = true; g_wpPos = {kSF.p.x, kSF.p.y, 0.0f};
    SyncWaypoint(kLV.p.x, kLV.p.y, kLV.p.z);
    CHECK(GetStatus() == NavStatus::Navigating && GetRoute().nodes.size() > 64, "SyncWaypoint: route without Tick");
    g_wpActive = false; Sleep(250);
    SyncWaypoint(kLV.p.x, kLV.p.y, kLV.p.z);
    CHECK(GetStatus() == NavStatus::Idle && GetRoute().nodes.empty(), "SyncWaypoint: removal clears route");
    flags::SetForTesting(flags::Flags{false, false, false});
    g_wpActive = true; Sleep(250);
    SyncWaypoint(kLV.p.x, kLV.p.y, kLV.p.z);
    CHECK(GetStatus() == NavStatus::Idle, "SyncWaypoint respects Core=0");

    SetWaypointSourceForTesting(nullptr);
    SetNativeSearchForTesting(nullptr);
    flags::SetForTesting(std::nullopt);
    Reset();
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    TestSyntheticSearch();
    TestRealData();
    TestPlanner();
    TestArrival();
    std::printf("\ngps routing: %d checks passed, %d failed, %d sections skipped\n", g_pass, g_fail, g_skip);
    return g_fail ? 1 : 0;
}
