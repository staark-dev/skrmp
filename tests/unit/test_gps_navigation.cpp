// API-003 FINAL — snapping, maneuvers, engine events, clipping, ESC MAP
// dashes, miter joins, adaptive zoom. Links the real client/gps sources.
// Real-data checks need gta3.img (SKR_GTA_IMG or the Steam default).
// Exit 0 = pass, 1 = fail.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "gps/gps_clip.h"
#include "gps/gps_destination.h"
#include "gps/gps_draw.h"
#include "gps/gps_engine.h"
#include "gps/gps_feedback.h"
#include "gps/gps_flags.h"
#include "gps/gps_graph.h"
#include "gps/gps_route.h"
#include "gps/gps_zoom.h"
#include "native-pause/map_route_geometry.h"

using namespace skr::gps;
namespace G = skr::gps::graph;
namespace F = skr::gps::feedback;
namespace Z = skr::gps::zoom;
namespace MR = skr::native_pause::map_route;

static int g_pass = 0, g_fail = 0, g_skip = 0;
#define CHECK(cond, msg) do { if (cond) ++g_pass; else { ++g_fail; std::printf("  FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

static G::RoadGraph g_real;
static bool g_realOk = false;

static void LoadReal() {
    std::wstring img = L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Grand Theft Auto San Andreas\\models\\gta3.img";
    if (const wchar_t* env = _wgetenv(L"SKR_GTA_IMG")) img = env;
    std::string err;
    g_realOk = GetFileAttributesW(img.c_str()) != INVALID_FILE_ATTRIBUTES && G::LoadFromImg(img, g_real, err);
    if (!g_realOk) { std::puts("  real road data unavailable: real-data sections skipped"); ++g_skip; }
}

static bool Linked(const G::RoadGraph& g, uint32_t a, uint32_t b) {
    const G::Node& n = g.nodes[a];
    for (uint32_t k = 0; k < n.linkCount; ++k) if (g.links[n.firstLink + k] == b) return true;
    return false;
}

// ── Snapping ────────────────────────────────────────────────────────
static void TestSnapping() {
    std::puts("== Destination snapping ==");
    // Synthetic: two networks; the nearest road to the waypoint is unreachable.
    std::vector<G::AreaFile> areas(G::kAreas);
    auto add = [&](float x, float y, uint8_t flood) {
        G::Node n; n.x = x; n.y = y; n.area = 0; n.id = (int16_t)areas[0].nodes.size();
        n.floodFill = flood; n.vehicle = true; areas[0].nodes.push_back(n); return n.id; };
    std::vector<std::vector<int>> adj;
    auto link = [&](int a, int b) { if ((int)adj.size() <= (std::max)(a, b)) adj.resize((std::max)(a, b) + 1);
                                    adj[a].push_back(b); adj[b].push_back(a); };
    int a0 = add(-2990, -2990, 1), a1 = add(-2700, -2990, 1), a2 = add(-2700, -2700, 1);
    int b0 = add(-2600, -2650, 2), b1 = add(-2400, -2650, 2);
    link(a0, a1); link(a1, a2); link(b0, b1);
    adj.resize(areas[0].nodes.size());
    for (size_t i = 0; i < adj.size(); ++i) {
        areas[0].nodes[i].firstLink = (uint32_t)areas[0].links.size();
        areas[0].nodes[i].linkCount = (uint8_t)adj[i].size();
        for (int t : adj[i]) areas[0].links.push_back({0, (uint16_t)t});
    }
    G::RoadGraph g; std::string err;
    CHECK(G::BuildGraph(areas, g, err), "synthetic graph");
    // Waypoint 30 from network 2, 100 from the end of network 1.
    G::SearchResult r = G::FindRoute(g, {-2990, -2990, 0}, {-2600, -2680, 0});
    CHECK(r.status == G::SearchStatus::Found && r.destReSnapped, "unreachable nearest road -> reachable road chosen");
    CHECK(std::fabs(r.end.x + 2700.0f) < 0.5f, "end on the reachable network");
    // Mid-segment snap: waypoint beside the long a0–a1 link, far from both nodes.
    r = G::FindRoute(g, {-2990, -2990, 0}, {-2850, -2970, 0});
    CHECK(r.status == G::SearchStatus::Found && r.destSnapDist < 21.0f, "mid-link waypoint snaps to the link, not a node 140 away");
    CHECK(r.path.empty() && std::fabs(r.length - 140.0f) < 0.5f, "same-link route: that piece of road only");

    if (!g_realOk) return;
    // Property on real data: snapping to links is never worse than to nodes.
    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> ux(-2900.0f, 2900.0f);
    int worse = 0, samples = 0;
    float maxGain = 0.0f;
    for (int i = 0; i < 400; ++i) {
        const Vec3 p{ux(rng), ux(rng), 0.0f};
        float nodeD = 0;
        const uint32_t node = G::NearestNode(g_real, p, false, 300.0f, &nodeD);
        const G::RoadPoint rp = G::NearestRoadPoint(g_real, p, 0.0f, 300.0f);
        if (node == G::kNoNode) continue;
        ++samples;
        if (!rp.Valid() || rp.dist > nodeD + 0.01f) ++worse;
        if (rp.Valid()) maxGain = (std::max)(maxGain, nodeD - rp.dist);
        if (rp.Valid()) CHECK(Linked(g_real, rp.a, rp.b), "snap point lies on a real link");
    }
    std::printf("  %d random samples: link snap worse than node snap %d times; max gain %.0f units\n",
                samples, worse, maxGain);
    CHECK(samples > 100 && worse == 0, "link snapping never worse than node snapping");

    // Deterministic: waypoint 10 units beside the middle of the longest real link.
    uint32_t la = G::kNoNode, lb = G::kNoNode; float longest = 0.0f;
    for (uint32_t a = 0; a < g_real.nodes.size(); ++a) {
        if (!g_real.Routable(a)) continue;
        const G::Node& na = g_real.nodes[a];
        for (uint32_t k = 0; k < na.linkCount; ++k) {
            const uint32_t b = g_real.links[na.firstLink + k];
            if (b <= a || !g_real.Routable(b)) continue;
            const float len = std::hypot(g_real.nodes[b].x - na.x, g_real.nodes[b].y - na.y);
            if (len > longest) { longest = len; la = a; lb = b; }
        }
    }
    const G::Node& A = g_real.nodes[la]; const G::Node& B = g_real.nodes[lb];
    const float ex = (B.x - A.x) / longest, ey = (B.y - A.y) / longest;
    const Vec3 beside{(A.x + B.x) * 0.5f - ey * 10.0f, (A.y + B.y) * 0.5f + ex * 10.0f, 0.0f};
    float nodeD = 0;
    G::NearestNode(g_real, beside, false, 300.0f, &nodeD);
    const G::RoadPoint mid = G::NearestRoadPoint(g_real, beside, 0.0f, 300.0f);
    std::printf("  longest link %.0f units: node snap %.0f vs link snap %.1f\n", longest, nodeD, mid.dist);
    CHECK(mid.Valid() && mid.dist <= 10.5f && nodeD > 40.0f, "mid-link waypoint: link snap removes the node-spacing gap");

    // Gant Bridge: SF downtown -> Bayside crosses x≈-2680, 1450 < y < 1950.
    r = G::FindRoute(g_real, {-1990.0f, 880.0f, 45.0f}, {-2485.0f, 2300.0f, 0.0f});
    bool bridge = false;
    for (uint32_t id : r.path) { const G::Node& n = g_real.nodes[id]; bridge |= n.x > -2760 && n.x < -2600 && n.y > 1450 && n.y < 1950; }
    CHECK(r.status == G::SearchStatus::Found && bridge, "SF -> Bayside uses the Gant Bridge");
    // Airports / highways: LS airport -> LV airport.
    r = G::FindRoute(g_real, {1685.0f, -2250.0f, 13.5f}, {1710.0f, 1450.0f, 0.0f});
    CHECK(r.status == G::SearchStatus::Found && r.path.size() > 100, "LS airport -> LV airport");
}

// ── Maneuvers ───────────────────────────────────────────────────────
static int SynLinks(int16_t area, int16_t node) { return area < 0 ? -1 : (node == 5 ? 3 : 2); }
static int NoGraph(int16_t, int16_t) { return -1; }
static std::vector<RouteNode> g_realRoute;
static int RealLinks(int16_t area, int16_t node) {
    const uint32_t id = g_real.Global(area, node);
    return id == G::kNoNode ? -1 : g_real.nodes[id].linkCount;
}

static void TestManeuvers() {
    std::puts("== Maneuvers ==");
    // North 100 m, junction (id 5), then west: a left turn.
    std::vector<RouteNode> nodes;
    for (int i = 0; i <= 5; ++i) nodes.push_back({{0.0f, 20.0f * i, 0.0f}, 0, (int16_t)i});
    for (int i = 1; i <= 5; ++i) nodes.push_back({{-20.0f * i, 100.0f, 0.0f}, 0, (int16_t)(10 + i)});
    F::ManeuverInfo m = F::NextManeuver(nodes, 1, {0.0f, 0.0f, 0.0f}, &SynLinks);
    CHECK(m.reliable && m.maneuver == F::Maneuver::Left && std::fabs(m.distance - 100.0f) < 1.0f, "north then west = left in 100 m");
    for (auto& n : nodes) if (n.nodeId > 10) n.pos.x = -n.pos.x;
    m = F::NextManeuver(nodes, 1, {0.0f, 0.0f, 0.0f}, &SynLinks);
    CHECK(m.maneuver == F::Maneuver::Right, "north then east = right");
    m = F::NextManeuver(nodes, 1, {0.0f, 0.0f, 0.0f}, &NoGraph);
    CHECK(!m.reliable && m.maneuver == F::Maneuver::None, "no graph -> no invented turn");
    m = F::NextManeuver(nodes, 1, {0.0f, 0.0f, 0.0f}, &SynLinks, 50.0f);
    CHECK(m.reliable && m.maneuver == F::Maneuver::None, "junction beyond horizon: reliable 'no turn', not unreliable");
    // A bend without a junction is not a maneuver.
    std::vector<RouteNode> bend = nodes;
    for (auto& n : bend) n.nodeId = n.nodeId == 5 ? 6 : n.nodeId;
    m = F::NextManeuver(bend, 1, {0.0f, 0.0f, 0.0f}, &SynLinks);
    CHECK(m.maneuver == F::Maneuver::Arrive, "curve without junction: only arrival ahead");

    if (!g_realOk) return;
    G::SearchResult r = G::FindRoute(g_real, {2495.0f, -1665.0f, 13.3f}, {1481.0f, -1741.0f, 0.0f});
    std::vector<RouteNode> route;
    for (uint32_t id : r.path) route.push_back({{g_real.nodes[id].x, g_real.nodes[id].y, g_real.nodes[id].z},
                                                g_real.nodes[id].area, g_real.nodes[id].id});
    int turns = 0;
    for (int i = 1; i < (int)route.size(); i += 3) {
        F::ManeuverInfo mi = F::NextManeuver(route, i, route[i].pos, &RealLinks, 400.0f);
        turns += mi.reliable && mi.maneuver != F::Maneuver::None && mi.maneuver != F::Maneuver::Arrive;
    }
    CHECK(r.status == G::SearchStatus::Found && turns > 0, "real LS route has junction maneuvers");
}

// ── Clipping, dashes, miters ────────────────────────────────────────
static void TestGeometry() {
    std::puts("== Radar clipping / ESC MAP dashes / miter joins ==");
    using clip::P2;
    std::vector<P2> out; std::vector<uint32_t> counts;
    const P2 across[] = {{-2.0f, 0.0f}, {2.0f, 0.0f}};
    clip::ClipPolyline(across, 2, clip::Region::UnitCircle, out, counts);
    CHECK(counts.size() == 1 && std::fabs(out[0].x + 1.0f) < 1e-4f && std::fabs(out[1].x - 1.0f) < 1e-4f, "circle clip: chord");
    const P2 corner[] = {{0.0f, 0.0f}, {0.95f, 0.95f}};
    clip::ClipPolyline(corner, 2, clip::Region::UnitCircle, out, counts);
    CHECK(counts.size() == 1 && std::hypot(out[1].x, out[1].y) <= 1.0001f, "circle clip: no drawing in square corners");
    clip::ClipPolyline(corner, 2, clip::Region::UnitSquare, out, counts);
    CHECK(counts.size() == 1 && std::fabs(out[1].x - 0.95f) < 1e-4f, "square keeps corners");
    const P2 inOutIn[] = {{0.0f, 0.0f}, {3.0f, 0.0f}, {3.0f, 0.5f}, {0.0f, 0.5f}};
    clip::ClipPolyline(inOutIn, 4, clip::Region::UnitSquare, out, counts);
    CHECK(counts.size() == 2 && counts[0] == 2 && counts[1] == 2, "leave and re-enter: two pieces, no bridging line");
    const P2 outside[] = {{3.0f, 3.0f}, {4.0f, 3.0f}};
    clip::ClipPolyline(outside, 2, clip::Region::UnitCircle, out, counts);
    CHECK(counts.empty(), "fully outside: nothing");
    const P2 joined[] = {{-0.5f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.5f}};
    clip::ClipPolyline(joined, 3, clip::Region::UnitCircle, out, counts);
    CHECK(counts.size() == 1 && counts[0] == 3, "inside polyline stays one joined piece");

    // Dashes: 100 px straight, dash 10 gap 5 -> 7 dashes (last partial).
    std::vector<MR::Pt> dp; std::vector<unsigned> dc;
    const MR::Pt line[] = {{0, 0}, {100, 0}};
    MR::BuildDashes(line, 2, 10.0f, 5.0f, {-10, -10}, {200, 10}, dp, dc);
    CHECK(dc.size() == 7, "dash count follows the pattern");
    // Pattern continues across a vertex: dash 0..10 on an L at 6 -> one bent dash.
    const MR::Pt l[] = {{0, 0}, {6, 0}, {6, 50}};
    MR::BuildDashes(l, 3, 10.0f, 5.0f, {-10, -10}, {100, 100}, dp, dc);
    CHECK(!dc.empty() && dc[0] == 3 && std::fabs(dp[2].y - 4.0f) < 1e-3f, "dash bends with the road at a vertex");
    float total = 0;
    size_t at = 0;
    for (unsigned c : dc) { for (unsigned k = 1; k < c; ++k) total += std::hypot(dp[at + k].x - dp[at + k - 1].x, dp[at + k].y - dp[at + k - 1].y); at += c; }
    CHECK(std::fabs(total - 40.0f) < 0.01f, "dash lengths exact over 56 px (4 x 10)");
    MR::BuildDashes(line, 2, 10.0f, 5.0f, {20, -10}, {50, 10}, dp, dc);
    bool inside = true;
    for (const auto& p : dp) inside &= p.x >= 20.0f - 1e-3f && p.x <= 50.0f + 1e-3f;
    CHECK(inside && !dc.empty(), "dashes clipped to the map rectangle");
    MR::Pt ca{-5, 5}, cb{5, 5};
    CHECK(MR::ClipToRect(ca, cb, {0, 0}, {10, 10}) && std::fabs(ca.x) < 1e-4f, "rect clip");

    // Miter: 90° corner -> half-width / cos45, capped at 2.5x for spikes.
    std::vector<ScreenPoint> strip;
    const ScreenPoint corner90[] = {{0, 0}, {10, 0}, {10, 10}};
    BuildStripOutline(corner90, 3, 2.0f, strip);
    const float mx = strip[2].x - strip[3].x, my = strip[2].y - strip[3].y;
    CHECK(strip.size() == 6 && std::fabs(std::hypot(mx, my) / 2.0f - 2.0f * 1.41421f) < 0.01f, "90° miter width");
    const ScreenPoint hairpin[] = {{0, 0}, {10, 0}, {0, 0.5f}};
    BuildStripOutline(hairpin, 3, 2.0f, strip);
    const float hx = strip[2].x - strip[3].x, hy = strip[2].y - strip[3].y;
    CHECK(std::hypot(hx, hy) / 2.0f <= 5.0f + 1e-3f, "hairpin miter capped (no spike)");
    const ScreenPoint dup[] = {{0, 0}, {0.1f, 0}, {10, 0}};
    BuildStripOutline(dup, 3, 2.0f, strip);
    CHECK(strip.size() == 4, "near-duplicate points dropped");
}

// ── Adaptive zoom ───────────────────────────────────────────────────
static void TestZoom() {
    std::puts("== Adaptive zoom ==");
    Z::Input in; in.navigating = true; in.dt = 1.0f / 60.0f;
    for (float s : {0.0f, 20.0f, 60.0f, 500.0f}) {
        in.speed = s;
        const float t = Z::TargetRange(in);
        CHECK(t >= Z::kMinRange && t <= Z::kMaxRange, "target within limits");
    }
    in.speed = 0.0f;  const float slow = Z::TargetRange(in);
    in.speed = 45.0f; const float fast = Z::TargetRange(in);
    CHECK(fast > slow + 150.0f, "zooms out with speed");
    in.junctionDistance = 80.0f;
    const float nearJunction = Z::TargetRange(in);
    CHECK(nearJunction < fast && nearJunction >= 80.0f, "zooms in near a junction but keeps it in view");
    in.junctionDistance = -1.0f;

    Z::Controller c;
    in.gtaRange = 180.0f; in.speed = 0.0f;
    float prev = c.Update(in), maxStep = 0.0f;
    in.speed = 45.0f;
    for (int i = 0; i < 600; ++i) { const float r = c.Update(in); maxStep = (std::max)(maxStep, std::fabs(r - prev)); prev = r; }
    CHECK(maxStep <= 160.0f / 60.0f + 1e-3f, "rate-limited (no jumps)");
    CHECK(std::fabs(prev - fast) <= fast * 0.041f, "converges to target (within the 4% band)");

    // Noisy speed around 20 u/s: after 3 s settling, the committed target
    // must stay put (no back-and-forth) within the band of the steady value.
    std::mt19937 rng(7); std::normal_distribution<float> noise(0.0f, 0.6f);
    in.speed = 20.0f;
    const float steady = Z::TargetRange(in);
    for (int i = 0; i < 180; ++i) { in.speed = 20.0f + noise(rng); c.Update(in); }
    int changes = 0, reversals = 0; float lastTarget = c.Target(), lastDelta = 0.0f;
    float lo = lastTarget, hi = lastTarget;
    for (int i = 0; i < 420; ++i) {
        in.speed = 20.0f + noise(rng);
        c.Update(in);
        const float d = c.Target() - lastTarget;
        if (d != 0.0f) {
            ++changes;
            if (lastDelta != 0.0f && (d > 0.0f) != (lastDelta > 0.0f)) ++reversals;
            lastDelta = d;
            lastTarget = c.Target();
        }
        lo = (std::min)(lo, lastTarget); hi = (std::max)(hi, lastTarget);
    }
    std::printf("  noisy speed (7 s settled): %d changes, %d reversals, spread %.1f around %.0f\n",
                changes, reversals, hi - lo, steady);
    CHECK(reversals <= 1 && hi - lo <= steady * 0.04f && std::fabs(lastTarget - steady) <= steady * 0.05f,
          "backlash: no oscillation under noisy speed");

    // Navigation ends: blend back to GTA's range, then release.
    in.navigating = false; in.gtaRange = 180.0f;
    bool released = false;
    for (int i = 0; i < 900 && !released; ++i) { c.Update(in); released = !c.Active(); }
    CHECK(released && c.Update(in) == 180.0f, "returns GTA range after navigation");
    Z::Controller idle;
    CHECK(idle.Update(in) == 180.0f && !idle.Active(), "never overrides when not navigating");
}

// ── Engine events / summary ─────────────────────────────────────────
static bool g_wpActive = false;
static Vec3 g_wpPos = {};
static bool FakeHas() { return g_wpActive; }
static Vec3 FakePos() { return g_wpPos; }
static bool FakeClear() { g_wpActive = false; return true; }
static int NoNative(const Vec3&, const Vec3&, std::vector<RouteNode>& out) { out.clear(); return 0; }
static std::vector<F::EventType> g_events;
static void OnEvent(const F::Event& e, void*) { g_events.push_back(e.type); }
static bool Saw(F::EventType t) { for (auto e : g_events) if (e == t) return true; return false; }
static void TickFor(const Vec3& p, int ms) { for (int t = 0; t < ms; t += 50) { Tick(p.x, p.y, p.z); Sleep(50); } }

static void TestEngine() {
    std::puts("== Engine events, reroute, cancel, origin retry ==");
    if (!g_realOk) { std::puts("  SKIPPED: needs real road data"); ++g_skip; return; }
    G::SetForTesting(&g_real);
    SetNativeSearchForTesting(&NoNative);
    flags::SetForTesting(flags::Flags{true, false, false, false});
    static const WaypointSource src{&FakeHas, &FakePos, &FakeClear};
    SetWaypointSourceForTesting(&src);
    const int sub = F::Subscribe(&OnEvent, nullptr);
    CHECK(sub > 0, "subscribe");
    Reset();

    const Vec3 grove{2495.0f, -1665.0f, 13.3f};
    g_wpActive = true; g_wpPos = {1481.0f, -1741.0f, 0.0f};
    TickFor(grove, 300);
    CHECK(Saw(F::EventType::RouteStarted) && F::GetSummary().state == F::Summary::State::Navigating, "route started");
    CHECK(F::GetSummary().remaining > 500.0f, "summary has remaining distance");

    // Leave the route: recalculating then rerouted (after the 2 s cooldown).
    g_events.clear();
    const Vec3 off{2400.0f, -1450.0f, 20.0f};
    TickFor(off, 2600);
    CHECK(Saw(F::EventType::Recalculating) && Saw(F::EventType::Rerouted), "off-route -> recalculating -> rerouted");
    CHECK(GetRoute().nodes.front().pos.DistanceTo2D(off) < 120.0f, "new route starts near the player");

    // New destination while navigating.
    g_events.clear();
    g_wpPos = {2229.0f, -1721.0f, 0.0f};
    TickFor(off, 300);
    CHECK(Saw(F::EventType::RouteStarted), "replacement destination starts a new route");

    // Cancel.
    g_events.clear();
    g_wpActive = false;
    TickFor(off, 300);
    CHECK(Saw(F::EventType::Cancelled) && GetRoute().nodes.empty() && F::GetSummary().state == F::Summary::State::Idle,
          "cancel clears the route immediately");

    // Unreachable destination: reason reported, waypoint kept.
    g_events.clear();
    g_wpActive = true; g_wpPos = {-2900.0f, -2900.0f, 0.0f};
    TickFor(grove, 300);
    CHECK(Saw(F::EventType::Unreachable) && F::GetSummary().state == F::Summary::State::Unavailable &&
          F::GetSummary().failure == RouteFailure::NoRoadNearDestination && g_wpActive, "unreachable: clear reason, waypoint kept");

    // Player inside an interior (far above the map): retried when back outside.
    g_events.clear();
    g_wpPos = {2229.0f, -1721.0f, 0.0f};
    const Vec3 interior{2495.0f, -1665.0f, 1013.0f};
    TickFor(interior, 300);
    CHECK(GetRoute().failure == RouteFailure::NoRoadNearOrigin, "interior: no road near player");
    TickFor(grove, 3300);
    CHECK(GetStatus() == NavStatus::Navigating, "origin failure retried once back on a road");

    // Off-road waypoint: leaving the road toward it must not count as off-route.
    g_events.clear();
    g_wpPos = {1929.0f, -509.0f, 0.0f};          // ~190 units from the nearest road (live log)
    TickFor(grove, 300);
    const RouteSnapshot& offRoad = GetRoute();
    CHECK(GetStatus() == NavStatus::Navigating && offRoad.destinationSnap > 100.0f, "off-road waypoint routed to the road");
    const Vec3 roadEnd = offRoad.roadDestination;
    const Vec3 halfway{(roadEnd.x + g_wpPos.x) * 0.5f, (roadEnd.y + g_wpPos.y) * 0.5f, roadEnd.z};
    g_events.clear();
    TickFor(halfway, 2600);
    CHECK(!Saw(F::EventType::Recalculating) && GetStatus() == NavStatus::Navigating,
          "driving off-road toward the waypoint: no recalculation");
    TickFor({g_wpPos.x + 5.0f, g_wpPos.y, roadEnd.z}, 300);
    CHECK(Saw(F::EventType::Arrived) && !g_wpActive, "arrives at the off-road waypoint");

    // Arrive (on-road destination).
    g_wpActive = true; g_wpPos = {2229.0f, -1721.0f, 0.0f};
    TickFor(grove, 300);
    g_events.clear();
    TickFor(GetRoute().nodes.back().pos, 300);
    CHECK(Saw(F::EventType::Arrived) && !g_wpActive && F::GetSummary().state == F::Summary::State::Arrived, "arrival");

    F::Unsubscribe(sub);
    g_events.clear();
    F::Emit({});
    CHECK(g_events.empty(), "unsubscribed listener not called");
    Reset();
    CHECK(F::GetSummary().state == F::Summary::State::Idle && GetRoute().nodes.empty(), "reset clears route and summary");

    SetWaypointSourceForTesting(nullptr);
    SetNativeSearchForTesting(nullptr);
    flags::SetForTesting(std::nullopt);
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    LoadReal();
    TestSnapping();
    TestManeuvers();
    TestGeometry();
    TestZoom();
    TestEngine();
    std::printf("\ngps navigation: %d checks passed, %d failed, %d sections skipped\n", g_pass, g_fail, g_skip);
    return g_fail ? 1 : 0;
}
