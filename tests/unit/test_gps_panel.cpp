// API-003.2 — GPS navigation panel (layout, alignment, truncation, distance
// text, lifecycle), thin route strokes, ESC MAP dash phase, notification
// policy, and a source scan for render-path I/O. Links the real client/gps
// sources. Exit 0 = pass, 1 = fail.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "gps/gps_draw.h"
#include "gps/gps_feedback.h"
#include "gps/gps_notify.h"
#include "gps/gps_panel_model.h"
#include "native-pause/map_route_draw.h"

using namespace skr::gps;
namespace P = skr::gps::panel;
namespace F = skr::gps::feedback;
namespace N = skr::gps::notify;
namespace MR = skr::native_pause::map_route;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { if (cond) ++g_pass; else { ++g_fail; std::printf("  FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

static bool Near(float a, float b, float eps = 0.01f) { return std::fabs(a - b) <= eps; }

// GTA's default radar slot: x = 40, w = 94, h = 76, top = 116 HUD units from
// the bottom of a 640×448 canvas (radar_hud.cpp, TASK-HUD-STATE v3).
static P::Rect DefaultRadar(float W, float H) {
    const float sx = W / 640.0f, sy = H / 448.0f;
    const float x = 40.0f * sx, y = H - 116.0f * sy;
    return {x, y, x + 94.0f * sx, y + 76.0f * sy};
}

static P::Layout LayoutFor(P::Rect radar, float W, float H, float ui = 1.0f) {
    P::LayoutInput in;
    in.radar = radar; in.screenW = W; in.screenH = H; in.uiScale = ui;
    return P::ComputeLayout(in);
}

// ── Anchoring ─────────────────────────────────────────────────────────
static void TestAnchoring() {
    std::puts("== Panel anchoring ==");
    for (float H : {768.0f, 1080.0f}) {
        const float W = H == 768.0f ? 1360.0f : 1920.0f;
        const P::Rect r = DefaultRadar(W, H);
        const P::Layout L = LayoutFor(r, W, H);
        std::printf("  %gx%g radar (%.1f,%.1f %.1fx%.1f) -> panel (%.0f,%.0f %.0fx%.0f) gap %.0f fonts %.0f/%.0f/%.0f\n",
                    W, H, r.x0, r.y0, r.Width(), r.Height(), L.panel.x0, L.panel.y0, L.panel.Width(),
                    L.panel.Height(), L.gap, L.labelFont, L.valueFont, L.detailFont);
        CHECK(L.visible, "default radar: panel visible");
        CHECK(std::fabs(L.panel.x0 - r.x0) <= 0.5f, "left edge = radar left edge");
        CHECK(std::fabs(L.panel.Width() - r.Width()) <= 1.0f, "width = radar width");
        const float gap = r.y0 - L.panel.y1;
        CHECK(gap >= 3.5f && gap <= 6.5f, "4-6 px above the radar");
        CHECK(L.panel.y1 <= r.y0, "never overlaps the radar");
        CHECK(L.row1.y0 >= L.panel.y0 && L.row2.y1 <= L.panel.y1 && Near(L.row1.y1, L.row2.y0), "rows stacked inside the panel");
        CHECK(L.accent.x0 == L.panel.x0 && L.accent.Width() >= 2.0f, "lime accent on the left edge");
    }
    // Same geometry when only the radar's viewed range changes (adaptive zoom
    // never touches the rect), and when the rect is identical frame to frame.
    const P::Layout a = LayoutFor(DefaultRadar(1360, 768), 1360, 768);
    const P::Layout b = LayoutFor(DefaultRadar(1360, 768), 1360, 768);
    CHECK(std::memcmp(&a.panel, &b.panel, sizeof(a.panel)) == 0, "stable geometry for the same radar rect");

    // Custom radar sizes follow the measured rect.
    const P::Rect narrow = {60, 520, 180, 740};          // tall narrow custom radar
    const P::Layout n = LayoutFor(narrow, 1360, 768);
    CHECK(n.visible && n.panel.x0 == 60 && n.panel.Width() == 120, "narrow custom radar: width follows");
    const P::Rect wide = {30, 600, 330, 740};            // wide rectangle
    const P::Layout w = LayoutFor(wide, 1360, 768);
    CHECK(w.visible && w.panel.Width() == 300, "wide custom radar: width follows");
    CHECK(Near(n.panel.Height(), w.panel.Height()), "panel height independent of radar size");

    // Screen-safe bounds.
    const P::Layout high = LayoutFor({40, 150, 240, 300}, 1360, 768);
    CHECK(!high.visible, "radar too high (chat area): panel hidden, never over the chat");
    const P::Layout off = LayoutFor({-20, 600, 180, 740}, 1360, 768);
    CHECK(off.visible && off.panel.x0 >= 0.0f, "kept on screen horizontally");
    CHECK(!LayoutFor({0, 0, 0, 0}, 1360, 768).visible, "no radar rect: hidden");

    // UI scale applied exactly once.
    const P::Layout s1 = LayoutFor(DefaultRadar(1360, 768), 1360, 768, 1.0f);
    const P::Layout s2 = LayoutFor(DefaultRadar(1360, 768), 1360, 768, 1.25f);
    CHECK(std::fabs(s2.valueFont / s1.valueFont - 1.25f) < 0.08f, "UI scale 1.25 -> fonts x1.25 (single application)");
    CHECK(Near(s2.panel.Width(), s1.panel.Width()), "UI scale never changes the radar-bound width");
    CHECK(s2.gap <= 6.0f && s1.gap >= 4.0f, "gap stays 4-6 px at any UI scale");
}

// ── Alignment ─────────────────────────────────────────────────────────
static void TestAlignment() {
    std::puts("== Text alignment ==");
    const P::Rect row = {100, 200, 300, 218};             // 18 px tall
    const P::TextPos l = P::AlignLeft(row, 9, 60, 12);
    CHECK(l.x == 109 && l.y == 203, "left aligned: x = left + pad, y centred ((18-12)/2)");
    const P::TextPos r = P::AlignRight(row, 7, 50, 14);
    CHECK(r.x == 243 && r.y == 202, "right aligned: x = right - pad - width, y centred");
    const P::TextPos odd = P::AlignLeft(row, 9, 60, 13);  // 2.5 px -> snapped
    CHECK(odd.y == 203 || odd.y == 202, "half-pixel centre snapped to a whole pixel");
    // Centre of text equals centre of rect within 0.5 px for many heights.
    bool centred = true;
    for (float h = 8; h <= 18; h += 0.5f) {
        const P::TextPos p = P::AlignRight(row, 7, 40, h);
        centred &= std::fabs((p.y + h * 0.5f) - (row.y0 + row.Height() * 0.5f)) <= 0.51f;
    }
    CHECK(centred, "vertical centre within 0.5 px for 8-18 px text");
}

// ── Truncation ────────────────────────────────────────────────────────
static float Mono(const char* t, size_t n) {          // 7 px per code point
    float w = 0;
    for (size_t i = 0; i < n; ++i) if ((static_cast<unsigned char>(t[i]) & 0xC0) != 0x80) w += 7.0f;
    return w;
}

static void TestTruncation() {
    std::puts("== Long location names ==");
    char out[96];
    P::FitText("Rodeo", 100, Mono, out, sizeof(out));
    CHECK(std::strcmp(out, "Rodeo") == 0, "fitting text unchanged");
    char loc[112];
    P::FormatLocation("Las Venturas Airport", "Las Venturas", loc, sizeof(loc));
    CHECK(std::strcmp(loc, "Las Venturas Airport \xC2\xB7 Las Venturas") == 0, "area \xC2\xB7 city");
    P::FitText(loc, 140, Mono, out, sizeof(out));
    CHECK(Mono(out, std::strlen(out)) <= 140 && std::strstr(out, "...") != nullptr, "long location truncated with ...");
    std::printf("  140 px: \"%s\"\n", out);
    // Cut right after the separator: never leaves a dangling "·" or half a UTF-8 char.
    P::FitText(loc, 7 * 24, Mono, out, sizeof(out));
    const size_t len = std::strlen(out);
    bool validUtf8 = true;
    for (size_t i = 0; i < len; ++i) {
        const unsigned char c = static_cast<unsigned char>(out[i]);
        if (c == 0xC2) validUtf8 &= i + 1 < len && static_cast<unsigned char>(out[i + 1]) == 0xB7;
    }
    CHECK(validUtf8 && std::strstr(out, "\xC2\xB7...") == nullptr && std::strstr(out, " ...") == nullptr,
          "no split UTF-8, no dangling separator");
    std::printf("  168 px: \"%s\"\n", out);
    P::FitText("Whetstone", 10, Mono, out, sizeof(out));
    CHECK(std::strcmp(out, "") == 0 || std::strcmp(out, "...") == 0, "absurdly narrow: never overflows");
    P::FormatLocation("", "", loc, sizeof(loc));
    CHECK(std::strcmp(loc, "Unknown area") == 0, "no zone: Unknown area");
    P::FormatLocation("Las Venturas", "Las Venturas", loc, sizeof(loc));
    CHECK(std::strcmp(loc, "Las Venturas") == 0, "area == city: shown once");
    P::FormatLocation("Bad\x01\xE9Name", "", loc, sizeof(loc));
    CHECK(std::strcmp(loc, "BadName") == 0, "non-ASCII / control bytes from GXT dropped");
}

// ── Distance text ─────────────────────────────────────────────────────
static std::string Fmt(P::DistanceDisplay& d) { char b[24]; d.Format(b, sizeof(b)); return b; }

static void TestDistance() {
    std::puts("== Distance formatting ==");
    P::DistanceDisplay d;
    d.Update(90.0f);    CHECK(Fmt(d) == "90 M", "90 M");
    d.Reset(); d.Update(291.0f);  CHECK(Fmt(d) == "290 M", "290 M");
    d.Reset(); d.Update(3304.0f); CHECK(Fmt(d) == "3.3 KM", "3.3 KM");
    d.Reset(); d.Update(12840.0f); CHECK(Fmt(d) == "12.8 KM", "12.8 KM");
    // Noise around a rounding boundary (295 ± 3) never flickers.
    d.Reset(); d.Update(296.0f);
    const std::string first = Fmt(d);
    bool stable = true;
    for (int i = 0; i < 200; ++i) { d.Update(295.0f + 3.0f * std::sin(i * 1.7f)); stable &= Fmt(d) == first; }
    CHECK(stable, "+-3 m noise at 295 m: text stable");
    // Real progress still updates promptly (within one step).
    d.Update(270.0f); CHECK(Fmt(d) == "270 M", "progress beyond hysteresis updates");
    // Unit switch with hysteresis.
    d.Reset(); d.Update(990.0f); d.Update(1004.0f); CHECK(Fmt(d) == "1.0 KM", "-> km at 1000 m");
    d.Update(985.0f); CHECK(Fmt(d) == "1.0 KM", "985 m stays km (hysteresis)");
    d.Update(940.0f); CHECK(Fmt(d) == "940 M", "back to m below 950");
    d.Reset(); CHECK(Fmt(d) == "--", "no distance: placeholder");
}

// ── Lifecycle ─────────────────────────────────────────────────────────
static F::Summary Sum(F::Summary::State st, float remaining, uint32_t since) {
    F::Summary s; s.state = st; s.remaining = remaining; s.stateSince = since; return s;
}

static void TestLifecycle() {
    std::puts("== Widget lifecycle ==");
    using S = F::Summary::State;
    P::Model m;
    m.Update(Sum(S::Idle, 0, 0), 1000);
    CHECK(!m.GetView().visible, "Idle: hidden");

    P::Model fresh;
    fresh.Update(Sum(S::Navigating, 500, 1000), 1000);
    CHECK(fresh.GetView().visible && fresh.GetView().detail[0] == '\0', "no zone lookup yet: empty row, no fake name");

    m.SetLocation("The High Roller", "Las Venturas");
    m.Update(Sum(S::Navigating, 3304, 1000), 1000);
    CHECK(m.GetView().visible && m.GetView().alpha == 0.0f, "Navigating: fades in from 0");
    m.Update(Sum(S::Navigating, 3304, 1000), 1000 + P::kFadeMs);
    const P::View& v = m.GetView();
    CHECK(v.alpha == 1.0f, "fully visible after the fade");
    CHECK(std::strcmp(v.label, "GPS ROUTE") == 0 && std::strcmp(v.value, "3.3 KM") == 0, "label + distance");
    CHECK(std::strcmp(v.detail, "The High Roller \xC2\xB7 Las Venturas") == 0, "location row");

    F::Summary turn = Sum(S::Navigating, 3290, 1000);
    turn.next.maneuver = F::Maneuver::Right; turn.next.distance = 121; turn.next.reliable = true;
    m.Update(turn, 1300);
    CHECK(std::strcmp(m.GetView().detail, "Turn right in 120 m") == 0, "reliable turn <= 250 m replaces location");
    turn.next.reliable = false;
    m.Update(turn, 1310);
    CHECK(std::strstr(m.GetView().detail, "High Roller") != nullptr, "unreliable maneuver: location");

    m.Update(Sum(S::Recalculating, 0, 1400), 1400);
    CHECK(m.GetView().visible && m.GetView().alpha == 1.0f && std::strcmp(m.GetView().label, "RECALCULATING") == 0,
          "Recalculating: visible without re-fade");
    CHECK(m.GetView().valueDimmed && std::strcmp(m.GetView().value, "3.3 KM") == 0, "last distance kept, dimmed");

    m.Update(Sum(S::Arrived, 0, 5000), 5000);
    CHECK(m.GetView().visible && std::strcmp(m.GetView().label, "ARRIVED") == 0 && m.GetView().tone == P::Tone::Success,
          "Arrived: success presentation");
    m.Update(Sum(S::Arrived, 0, 5000), 5000 + P::kArrivedShowMs - P::kFadeMs / 2);
    CHECK(m.GetView().alpha < 1.0f, "Arrived fades before it ends");
    m.Update(Sum(S::Arrived, 0, 5000), 5000 + P::kArrivedShowMs);
    m.Update(Sum(S::Arrived, 0, 5000), 5000 + P::kArrivedShowMs + P::kFadeMs);
    CHECK(!m.GetView().visible, "Arrived: hidden afterwards (route + panel cleared)");

    // Cancel mid-route: hidden within one fade.
    m.Update(Sum(S::Navigating, 800, 9000), 9000);
    m.Update(Sum(S::Navigating, 800, 9000), 9000 + P::kFadeMs);
    m.Update(Sum(S::Idle, 0, 9500), 9500);
    m.Update(Sum(S::Idle, 0, 9500), 9500 + P::kFadeMs);
    CHECK(!m.GetView().visible, "Cancelled: hidden");
    m.Update(Sum(S::Navigating, 800, 9800), 9800 + P::kFadeMs);
    CHECK(std::strcmp(m.GetView().value, "800 M") == 0, "distance does not leak from the previous route");

    F::Summary fail = Sum(S::Unavailable, 0, 20000);
    fail.failure = RouteFailure::NoRoadNearDestination;
    m.Update(fail, 20000 + P::kFadeMs);
    CHECK(m.GetView().visible && m.GetView().tone == P::Tone::Error && std::strcmp(m.GetView().label, "NO ROUTE") == 0,
          "Unreachable: concise error");
    CHECK(std::strcmp(m.GetView().detail, "No road near destination") == 0, "failure reason, capitalised");
    m.Update(fail, 20000 + P::kErrorShowMs);
    m.Update(fail, 20000 + P::kErrorShowMs + P::kFadeMs);
    CHECK(!m.GetView().visible, "Unreachable: hidden afterwards");
}

// ── Stroke widths ─────────────────────────────────────────────────────
// Across a horizontal line, the outer rail sits at width/2 + fringe/2 and the
// solid core at width/2 - fringe/2: 50 % coverage = the nominal width.
static float RailHalf(float half) {
    std::vector<ScreenPoint> s;
    const ScreenPoint line[] = {{0, 0}, {100, 0}};
    BuildStripOutline(line, 2, half, s);
    return std::fabs(s[0].y - s[1].y) * 0.5f;
}

static void TestStrokes() {
    std::puts("== Route stroke widths ==");
    const RouteStyle s768 = MinimapRouteStyle(768), s1080 = MinimapRouteStyle(1080), s600 = MinimapRouteStyle(600);
    std::printf("  before: fill 7.0 px (half 3.5), outline 11.0 px (half 5.5) at every resolution\n");
    std::printf("  after : 768p fill %.2f / outline %.2f px; 1080p fill %.2f / outline %.2f px\n",
                s768.fillWidth, s768.outlineWidth, s1080.fillWidth, s1080.outlineWidth);
    CHECK(s768.fillWidth >= 1.8f && s768.fillWidth <= 2.3f && s1080.fillWidth >= 1.8f && s1080.fillWidth <= 2.3f,
          "route 1.8-2.3 px");
    CHECK(s768.outlineWidth >= 2.8f && s768.outlineWidth <= 3.2f && s1080.outlineWidth >= 2.8f && s1080.outlineWidth <= 3.21f,
          "outline 2.8-3.2 px");
    CHECK(s600.fillWidth == s768.fillWidth, "never thinner than the 768p weight");
    CHECK(s1080.fillWidth / s768.fillWidth <= 1.16f, "1080p at most 15 % heavier");
    const float core = RailHalf((s768.fillWidth - kFringe) * 0.5f), outer = RailHalf((s768.fillWidth - kFringe) * 0.5f + kFringe);
    CHECK(Near(core + outer, s768.fillWidth), "core + fringe rails average to the nominal width (no double scaling)");
    CHECK(((s768.fillColor >> 16) & 0xFF) == 0xA3 && ((s768.fillColor >> 8) & 0xFF) == 0xE6 && (s768.fillColor & 0xFF) == 0x5B,
          "muted lime #A3E65B");
    CHECK((s768.outlineColor >> 24) < 0xA0, "dark outline at low opacity");

    // Joins.
    std::vector<ScreenPoint> a, b;
    const ScreenPoint corner90[] = {{0, 0}, {10, 0}, {10, 10}};
    BuildStripOutline(corner90, 3, 1.0f, a);
    CHECK(a.size() == 6, "90 deg: single mitred pair");
    const ScreenPoint sharp[] = {{0, 0}, {20, 0}, {2, 6}};   // ~162 deg turn
    BuildStripOutline(sharp, 3, 1.0f, a);
    bool bounded = a.size() == 8;
    for (size_t i = 2; i < 6; ++i) bounded &= std::hypot(a[i].x - 20, a[i].y) <= 1.0f * kMiterLimit + 1e-3f;
    CHECK(bounded, "sharp turn: bevel (2 pairs), nothing beyond the miter limit");
    BuildStripOutline(sharp, 3, 2.0f, b);
    CHECK(a.size() == b.size(), "pair count independent of width (core and fringe rails align)");
    const ScreenPoint hairpin[] = {{0, 0}, {10, 0}, {0, 0.5f}};
    BuildStripOutline(hairpin, 3, 1.0f, a);
    bool noSpike = true;
    for (const auto& p : a) noSpike &= std::hypot(p.x - 10, p.y) <= 2.0f + 1e-3f || p.x < 9.0f;
    CHECK(noSpike, "hairpin: no spike");
}

// ── ESC MAP dashes ────────────────────────────────────────────────────
static void TestDashes() {
    std::puts("== ESC MAP dashes ==");
    const MR::MapRouteStyle m768 = MR::StyleForScreen(768), m1080 = MR::StyleForScreen(1080);
    std::printf("  before: dash 3.5 / underlay 7.0 px at 1080p (Scale), dash 12 gap 7\n");
    std::printf("  after : 768p dash %.2f / underlay %.2f px, dash %.1f gap %.1f; 1080p dash %.2f / underlay %.2f\n",
                m768.dashWidth, m768.underlayWidth, m768.dash, m768.gap, m1080.dashWidth, m1080.underlayWidth);
    CHECK(m768.dashWidth >= 1.5f && m1080.dashWidth <= 2.0f + 1e-3f, "dash 1.5-2.0 px");
    CHECK(m768.underlayWidth - m768.dashWidth <= 1.2f, "underlay only slightly wider");

    // Phase continuity: a route drawn from a later vertex with startPhase =
    // travelled length produces exactly the tail of the full route's dashes.
    const MR::Pt route[] = {{0, 0}, {37, 0}, {37, 41}, {90, 41}, {90, 120}};
    std::vector<MR::Pt> full, tail;
    std::vector<unsigned> fc, tc;
    const MR::Pt lo{-1000, -1000}, hi{1000, 1000};
    MR::BuildDashes(route, 5, 7, 5, lo, hi, full, fc);
    MR::BuildDashes(route + 2, 3, 7, 5, lo, hi, tail, tc, 37.0f + 41.0f);
    // Every tail dash point must coincide with a point of a full-route dash.
    bool same = !tail.empty();
    for (const auto& p : tail) {
        bool found = false;
        for (const auto& q : full) found |= std::fabs(p.x - q.x) < 1e-3f && std::fabs(p.y - q.y) < 1e-3f;
        same &= found;
    }
    CHECK(same, "dash phase continuous when the visible start advances");
    // Total dashed length over the whole route = 7/12 of its length (± one dash).
    float dashed = 0; size_t at = 0;
    for (unsigned c : fc) { for (unsigned k = 1; k < c; ++k) dashed += std::hypot(full[at + k].x - full[at + k - 1].x, full[at + k].y - full[at + k - 1].y); at += c; }
    const float length = 37 + 41 + 53 + 79;
    CHECK(std::fabs(dashed - length * 7.0f / 12.0f) <= 7.0f, "consistent dash/gap rhythm across vertices");
    MR::BuildDashes(route, 5, 7, 5, lo, hi, full, fc, -3.0f);
    CHECK(!fc.empty(), "negative phase handled");
}

// ── Notifications ─────────────────────────────────────────────────────
static int g_delivered = 0;
static N::Kind g_lastKind = N::Kind::Navigation;
static void CountSink(const N::Notice& n, void*) { ++g_delivered; g_lastKind = n.kind; }

static void TestNotifications() {
    std::puts("== GPS notifications ==");
    using T = F::EventType;
    N::Policy p;
    N::Notice n;
    F::Event e; e.type = T::RouteStarted; e.distance = 3304;
    CHECK(p.Accept(e, 1000, n) && std::strstr(n.text, "3.3 km") && n.kind == N::Kind::Navigation, "route started: info");
    CHECK(!p.Accept(e, 2000, n), "waypoint moved within 4 s: coalesced");
    CHECK(!p.Accept(e, 5500, n), "still coalescing while the waypoint keeps moving");
    CHECK(p.Accept(e, 10000, n), "new destination after a pause: notified");
    e.type = T::Recalculating; CHECK(!p.Accept(e, 11000, n), "recalculating: silent");
    e.type = T::Rerouted;      CHECK(p.Accept(e, 11100, n), "rerouted: short status");
    CHECK(!p.Accept(e, 15000, n) && !p.Accept(e, 30000, n), "reroute storm: one notice per 20 s");
    CHECK(p.Accept(e, 31200, n), "reroute after 20 s");
    e.type = T::Cancelled;     CHECK(!p.Accept(e, 32000, n), "cancelled: silent");
    e.type = T::Arrived;       CHECK(p.Accept(e, 33000, n) && n.kind == N::Kind::Success, "arrived: success");
    CHECK(!p.Accept(e, 33100, n), "arrived: no duplicate");
    e.type = T::Unreachable; e.failure = RouteFailure::NoRoadNearDestination;
    CHECK(p.Accept(e, 40000, n) && n.kind == N::Kind::Error, "unreachable: error");
    CHECK(!p.Accept(e, 45000, n) && !p.Accept(e, 60000, n), "same failure: not repeated within 30 s");
    CHECK(p.Accept(e, 71000, n), "same failure after 30 s");

    // Through the real feedback bus: N events, exactly the accepted ones delivered once.
    N::Install();
    N::Install();                         // idempotent: still one subscription
    N::SetSink(&CountSink, nullptr);
    N::Reset();
    const N::Stats before = N::GetStats();
    F::Event s; s.type = T::RouteStarted; s.distance = 500;
    for (int i = 0; i < 5; ++i) F::Emit(s);                  // rapid waypoint drags
    F::Event r; r.type = T::Rerouted;
    for (int i = 0; i < 5; ++i) F::Emit(r);
    F::Event a; a.type = T::Arrived;
    F::Emit(a); F::Emit(a);
    const N::Stats after = N::GetStats();
    CHECK(g_delivered == 3, "12 events -> 3 notices (start, reroute, arrival)");
    CHECK(after.delivered - before.delivered == 3 && after.suppressed - before.suppressed == 9, "stats match deliveries");
    CHECK(g_lastKind == N::Kind::Success, "last notice is the arrival");
    N::SetSink(nullptr, nullptr);
    F::Emit(a);
    CHECK(g_delivered == 3, "no sink installed: nothing dispatched");
}

// ── Display policy (driver / on foot / passenger) ─────────────────────
static void TestPolicy() {
    std::puts("== Display policy ==");
    using M = P::DisplayMode; using St = P::Seat;
    CHECK(P::DesiredMode(true, St::Driver, P::ModeOverride::Auto) == M::Full, "driver -> full");
    CHECK(P::DesiredMode(true, St::OnFoot, P::ModeOverride::Auto) == M::Compact, "on foot -> compact");
    CHECK(P::DesiredMode(true, St::Passenger, P::ModeOverride::Auto) == M::Compact, "passenger -> compact");
    CHECK(P::DesiredMode(false, St::Driver, P::ModeOverride::Auto) == M::Hidden, "no navigation -> hidden");
    CHECK(P::DesiredMode(true, St::OnFoot, P::ModeOverride::ForceFull) == M::Full, "override full");
    CHECK(P::DesiredMode(true, St::Driver, P::ModeOverride::ForceCompact) == M::Compact, "override compact");
    CHECK(P::DesiredMode(true, St::Driver, P::ModeOverride::ForceHidden) == M::Hidden, "override hidden");

    P::DisplayPolicy p;
    CHECK(p.Update(true, St::OnFoot, 1000) == M::Compact, "first show is immediate");
    CHECK(p.Update(true, St::Driver, 1100) == M::Compact, "enter driver seat: held for the switch delay");
    CHECK(p.Update(true, St::Driver, 1100 + P::kSwitchDelayMs - 1) == M::Compact, "still held just before the delay");
    CHECK(p.Update(true, St::Driver, 1100 + P::kSwitchDelayMs) == M::Full, "driver after the delay -> full");
    // Noisy samples (enter/exit animation, one odd read) never switch.
    bool stable = true;
    for (uint32_t t = 2000; t < 4000; t += 100) stable &= p.Update(true, (t / 100) % 2 ? St::OnFoot : St::Driver, t) == M::Full;
    CHECK(stable, "alternating seat samples < 300 ms apart: no flicker");
    CHECK(p.Update(false, St::Driver, 4100) == M::Hidden, "waypoint gone: hidden at once");
    CHECK(p.Update(true, St::Passenger, 4200) == M::Compact, "passenger: compact at once from hidden");
}

// ── Compact box layout ────────────────────────────────────────────────
static void TestCompactLayout() {
    std::puts("== Compact box layout ==");
    for (float H : {768.0f, 1080.0f}) {
        const float W = H == 768.0f ? 1360.0f : 1920.0f;
        P::LayoutInput in; in.radar = DefaultRadar(W, H); in.screenW = W; in.screenH = H;
        const P::CompactLayout c = P::ComputeCompactLayout(in, true, 34.0f * H / 768.0f);
        const P::Rect& r = in.radar;
        const float cx = (r.x0 + r.x1) * 0.5f, cy = (r.y0 + r.y1) * 0.5f, a = r.Width() * 0.5f, b = r.Height() * 0.5f;
        auto inside = [&](float x, float y) { return ((x - cx) / a) * ((x - cx) / a) + ((y - cy) / b) * ((y - cy) / b) <= 1.0f; };
        std::printf("  %gx%g round radar: box (%.0f,%.0f %.0fx%.0f) font %.0f\n", W, H, c.box.x0, c.box.y0,
                    c.box.Width(), c.box.Height(), c.font);
        CHECK(c.visible, "compact visible on the default radar");
        CHECK(inside(c.box.x0, c.box.y0) && inside(c.box.x1, c.box.y0) && inside(c.box.x0, c.box.y1) && inside(c.box.x1, c.box.y1),
              "all four corners inside the round radar");
        CHECK(std::fabs((c.box.x0 + c.box.x1) * 0.5f - cx) <= 1.0f, "horizontally centred");
        CHECK(c.box.y1 < cy - 8.0f, "upper half, clear of the player arrow");
        CHECK(c.box.y0 - r.y0 < r.Height() * 0.2f, "near the upper edge");
        CHECK(c.box.Width() < r.Width() * 0.45f, "small");
    }
    P::LayoutInput sq; sq.radar = {85, 545, 234, 699}; sq.screenW = 1360; sq.screenH = 768;
    const P::CompactLayout s = P::ComputeCompactLayout(sq, false, 34.0f);
    CHECK(s.visible && s.box.y0 >= sq.radar.y0 + 2.0f && s.box.y0 <= sq.radar.y0 + 4.0f, "square radar: just inside the top edge");
    const P::CompactLayout wide = P::ComputeCompactLayout(sq, false, 400.0f);
    CHECK(!wide.visible, "text wider than the radar: not drawn (never spills off the radar)");
}

// ── Mode transitions through the model ────────────────────────────────
static P::NavContext Ctx(P::Seat seat, float dist, bool waypoint = true) {
    P::NavContext c; c.seat = seat; c.hasWaypoint = waypoint; c.waypointDistance = dist; return c;
}

static void TestModes() {
    std::puts("== Driving / walking / passenger / transitions ==");
    using S = F::Summary::State; using St = P::Seat;
    P::Model m;
    m.SetLocation("Ganton", "Los Santos");
    uint32_t t = 1000;
    bool overlap = false;
    auto step = [&](const F::Summary& s, const P::NavContext& c) {
        m.Update(s, c, t);
        overlap |= m.GetView().visible && m.GetCompact().visible;
        t += 16;
    };

    // Walking: compact straight-line distance only.
    for (int i = 0; i < 20; ++i) step(Sum(S::Navigating, 1180, 1000), Ctx(St::OnFoot, 912));
    CHECK(m.Mode() == P::DisplayMode::Compact, "walking: compact mode");
    CHECK(m.GetCompact().visible && m.GetCompact().alpha == 1.0f && std::strcmp(m.GetCompact().value, "910 M") == 0,
          "walking: straight-line distance (912 -> 910 M), not the 1.2 km road distance");
    CHECK(!m.GetView().visible, "walking: full panel hidden");

    // Enter the driver's seat: compact fades out, then the full panel fades in.
    const uint32_t enter = t;
    for (int i = 0; i < 60; ++i) step(Sum(S::Navigating, 1180, 1000), Ctx(St::Driver, 912));
    CHECK(m.Mode() == P::DisplayMode::Full && m.GetView().visible && !m.GetCompact().visible, "driving: full panel only");
    CHECK(std::strcmp(m.GetView().value, "1.2 KM") == 0 && std::strstr(m.GetView().detail, "Ganton"), "driving: route distance + location");
    CHECK(t - enter < P::kSwitchDelayMs + 2 * P::kFadeMs + 400, "switch completes within delay + two fades");

    // Passenger: compact.
    for (int i = 0; i < 60; ++i) step(Sum(S::Navigating, 1100, 1000), Ctx(St::Passenger, 640));
    CHECK(m.Mode() == P::DisplayMode::Compact && m.GetCompact().visible && !m.GetView().visible, "passenger: compact only");

    // Passenger -> on foot: same mode, no transition.
    for (int i = 0; i < 30; ++i) step(Sum(S::Navigating, 1100, 1000), Ctx(i % 3 ? St::OnFoot : St::Passenger, 640));
    CHECK(m.Mode() == P::DisplayMode::Compact && m.GetCompact().alpha == 1.0f, "passenger -> foot: no transition, no fade");
    CHECK(!overlap, "full panel and compact box never visible together");

    // Unreachable road route on foot: the straight-line box still guides.
    F::Summary un = Sum(S::Unavailable, 0, t); un.failure = RouteFailure::NoRoadNearDestination;
    for (int i = 0; i < 10; ++i) step(un, Ctx(St::OnFoot, 300));
    CHECK(m.GetCompact().visible && std::strcmp(m.GetCompact().value, "300 M") == 0, "on foot, no road route: distance still shown");

    // Arrival on foot: compact cleared, no full panel pops up.
    for (int i = 0; i < 20; ++i) step(Sum(S::Arrived, 0, t), Ctx(St::OnFoot, -1, false));
    CHECK(!m.GetCompact().visible && !m.GetView().visible, "arrival on foot: both hidden");

    // Driving arrival: brief ARRIVED, then nothing.
    P::Model d;
    d.Update(Sum(S::Navigating, 400, 1), Ctx(St::Driver, 300), 1);
    d.Update(Sum(S::Navigating, 400, 1), Ctx(St::Driver, 300), 1 + P::kFadeMs);
    d.Update(Sum(S::Arrived, 0, 5000), Ctx(St::Driver, -1, false), 5000);
    CHECK(d.GetView().visible && std::strcmp(d.GetView().label, "ARRIVED") == 0 && !d.GetCompact().visible, "driving arrival: ARRIVED panel");
    d.Update(Sum(S::Idle, 0, 9000), Ctx(St::Driver, -1, false), 9000);
    d.Update(Sum(S::Idle, 0, 9000), Ctx(St::Driver, -1, false), 9000 + P::kFadeMs);
    CHECK(!d.GetView().visible && !d.GetCompact().visible && d.Mode() == P::DisplayMode::Hidden, "after arrival: no widget");

    // Cancel while walking: hidden within one fade.
    P::Model w;
    w.Update(Sum(S::Navigating, 900, 1), Ctx(St::OnFoot, 500), 1);
    w.Update(Sum(S::Idle, 0, 2000), Ctx(St::OnFoot, -1, false), 2000);
    w.Update(Sum(S::Idle, 0, 2000), Ctx(St::OnFoot, -1, false), 2000 + P::kFadeMs);
    CHECK(!w.GetCompact().visible, "cancel on foot: hidden");

    // No waypoint at all: nothing, whatever the seat.
    P::Model n;
    for (St seat : {St::OnFoot, St::Driver, St::Passenger}) n.Update(Sum(S::Idle, 0, 0), Ctx(seat, -1, false), 100);
    CHECK(!n.GetView().visible && !n.GetCompact().visible, "no route: no widget");

    // API-003.3 hook.
    P::Model o;
    o.SetOverride(P::ModeOverride::ForceHidden);
    o.Update(Sum(S::Navigating, 900, 1), Ctx(St::Driver, 500), 1);
    CHECK(!o.GetView().visible && !o.GetCompact().visible, "override hidden: nothing drawn");
}

// ── No render-path I/O (source scan) ──────────────────────────────────
static std::string ReadSource(const char* rel) {
    std::string path = __FILE__;
    path = path.substr(0, path.find_last_of("\\/"));   // tests/unit
    path += "/../../";
    path += rel;
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

static void TestNoRenderIo() {
    std::puts("== Render path: no I/O ==");
    const char* files[] = {"client/gps/gps_panel.cpp", "client/gps/gps_panel_model.h", "client/gps/gps_draw.cpp",
                           "client/gps/gps_display_policy.h",
                           "client/native-pause/map_route_draw.h", "client/native-pause/map_route_geometry.h"};
    const char* banned[] = {"fopen", "CreateFile", "ofstream", "ifstream", "OutputDebugString", "Log(", "printf(",
                            "GetPrivateProfile", "Sleep(", "WaitFor"};
    for (const char* f : files) {
        const std::string src = ReadSource(f);
        CHECK(!src.empty(), f);
        bool clean = true;
        for (const char* b : banned) {
            size_t at = 0;
            while ((at = src.find(b, at)) != std::string::npos) {
                // snprintf / vsnprintf are formatting into memory, not I/O.
                const bool isSn = std::strcmp(b, "printf(") == 0 && at >= 2 && src.compare(at - 2, 2, "sn") == 0;
                if (!isSn) { clean = false; std::printf("  %s: %s\n", f, b); }
                ++at;
            }
        }
        CHECK(clean, "no file / log / blocking call");
    }
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    TestAnchoring();
    TestAlignment();
    TestTruncation();
    TestDistance();
    TestLifecycle();
    TestStrokes();
    TestDashes();
    TestNotifications();
    TestPolicy();
    TestCompactLayout();
    TestModes();
    TestNoRenderIo();
    std::printf("\ngps panel: %d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
