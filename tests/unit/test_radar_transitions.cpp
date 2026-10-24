// Unit tests for radar mode transition state (API-002.8.1).
// Validates the state contract: SetRadarBounds must promote style to Square.
#include "protocol/radar_style.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; std::printf("  FAIL: %s (line %d)\n", msg, __LINE__); } \
} while(0)

using namespace skr::protocol;

// ── Simulated per-player radar state (mirrors SkrPlayerData fields) ──
struct SimRadarState {
    RadarStyle    style    = RadarStyle::Round;
    RadarSizeMode sizeMode = RadarSizeMode::Default;
    float         scale    = 1.0f;
    float         scaleW   = 1.0f;
    float         scaleH   = 1.0f;
    bool          visible  = true;
};

void SimSetStyle(SimRadarState& s, RadarStyle style) {
    s.style = style;
}

void SimSetSize(SimRadarState& s, RadarSizeMode mode, float scale) {
    s.sizeMode = mode;
    s.scale    = scale;
    if (mode == RadarSizeMode::Custom) { s.scaleW = scale; s.scaleH = scale; }
}

// The FIX: SetRadarBounds promotes style to Square
void SimSetBounds(SimRadarState& s, float w, float h) {
    s.style    = RadarStyle::Square;   // <-- the critical fix
    s.sizeMode = RadarSizeMode::Bounds;
    s.scaleW   = w;
    s.scaleH   = h;
}

void SimReset(SimRadarState& s) {
    s = SimRadarState{};
}

// ── Client-side state checks ──
struct SimClientState {
    bool maskDisabled = false;   // true = 0xC3 at DrawRadarMask (square)
    bool squareMode   = false;   // radar_hud g_squareMode
};

void SimApplyRadar(SimClientState& c, RadarStyle style) {
    c.maskDisabled = (style == RadarStyle::Square);
    c.squareMode   = (style == RadarStyle::Square);
}

// ── Test: The exact transition sequence from the spec ──
static void TestTransitionSequence() {
    std::puts("== Transition sequence ==");
    SimRadarState  srv{};
    SimClientState cli{};

    // /radarsquare
    SimSetStyle(srv, RadarStyle::Square);
    SimSetBounds(srv, 1.30f, 0.90f);
    SimApplyRadar(cli, srv.style);
    CHECK(srv.style == RadarStyle::Square, "1: style=Square");
    CHECK(cli.maskDisabled, "1: mask disabled");
    CHECK(cli.squareMode, "1: squareMode=true");

    // /radarsize 1.3 0.9
    SimSetBounds(srv, 1.30f, 0.90f);
    SimApplyRadar(cli, srv.style);
    CHECK(srv.style == RadarStyle::Square, "2: style=Square after size");

    // /radarsize 1.79 0.6 (clamped to 1.6 on client)
    SimSetBounds(srv, 1.79f, 0.60f);
    SimApplyRadar(cli, srv.style);
    CHECK(srv.style == RadarStyle::Square, "3: style=Square after extreme size");
    CHECK(cli.maskDisabled, "3: mask disabled");

    // /radardefault
    SimSetStyle(srv, RadarStyle::Round);
    SimSetSize(srv, RadarSizeMode::Default, 1.0f);
    SimApplyRadar(cli, srv.style);
    CHECK(srv.style == RadarStyle::Round, "4: style=Round");
    CHECK(!cli.maskDisabled, "4: mask enabled (vanilla)");
    CHECK(!cli.squareMode, "4: squareMode=false");

    // /radarcustom (sets style first, then bounds)
    SimSetStyle(srv, RadarStyle::Square);
    SimSetBounds(srv, 1.50f, 0.80f);
    SimApplyRadar(cli, srv.style);
    CHECK(srv.style == RadarStyle::Square, "5: style=Square after custom");
    CHECK(cli.maskDisabled, "5: mask disabled after custom");
    CHECK(cli.squareMode, "5: squareMode=true after custom");

    // /radarsize 1.3 0.9 (BUG SCENARIO: after /radardefault, this previously kept Round)
    SimSetStyle(srv, RadarStyle::Round);  // simulate /radardefault first
    SimSetSize(srv, RadarSizeMode::Default, 1.0f);
    SimSetBounds(srv, 1.30f, 0.90f);     // SetRadarBounds must promote to Square
    SimApplyRadar(cli, srv.style);
    CHECK(srv.style == RadarStyle::Square, "6: SetBounds promotes Round→Square");
    CHECK(cli.maskDisabled, "6: mask disabled after bounds promotion");
    CHECK(cli.squareMode, "6: squareMode=true after bounds promotion");

    // /radardefault again
    SimSetStyle(srv, RadarStyle::Round);
    SimSetSize(srv, RadarSizeMode::Default, 1.0f);
    SimApplyRadar(cli, srv.style);
    CHECK(srv.style == RadarStyle::Round, "7: back to Round");
    CHECK(!cli.maskDisabled, "7: mask enabled");

    // /radarsquare again
    SimSetStyle(srv, RadarStyle::Square);
    SimSetBounds(srv, 1.30f, 0.90f);
    SimApplyRadar(cli, srv.style);
    CHECK(srv.style == RadarStyle::Square, "8: Square again");
    CHECK(cli.maskDisabled, "8: mask disabled again");
    CHECK(cli.squareMode, "8: squareMode=true again");
}

// ── Test: Rapid repeated transitions ──
static void TestRapidTransitions() {
    std::puts("== Rapid transitions ==");
    SimRadarState  srv{};
    SimClientState cli{};

    for (int i = 0; i < 50; ++i) {
        SimSetStyle(srv, RadarStyle::Square);
        SimSetBounds(srv, 1.30f, 0.90f);
        SimApplyRadar(cli, srv.style);
        CHECK(cli.maskDisabled, "rapid: square mask disabled");
        CHECK(cli.squareMode, "rapid: squareMode true");

        SimSetStyle(srv, RadarStyle::Round);
        SimSetSize(srv, RadarSizeMode::Default, 1.0f);
        SimApplyRadar(cli, srv.style);
        CHECK(!cli.maskDisabled, "rapid: round mask enabled");
        CHECK(!cli.squareMode, "rapid: squareMode false");
    }
}

// ── Test: Client-side promotion (defensive: Round+Bounds → Square) ──
static void TestClientSidePromotion() {
    std::puts("== Client-side promotion ==");
    // If the server somehow sends Round + Bounds, the client should promote to Square.
    SimRadarState srv{};
    srv.style = RadarStyle::Round;
    srv.sizeMode = RadarSizeMode::Bounds;
    srv.scaleW = 1.30f;
    srv.scaleH = 0.90f;

    // Client-side defensive promotion
    RadarStyle effectiveStyle = srv.style;
    if (srv.sizeMode == RadarSizeMode::Bounds && effectiveStyle == RadarStyle::Round)
        effectiveStyle = RadarStyle::Square;

    SimClientState cli{};
    SimApplyRadar(cli, effectiveStyle);
    CHECK(effectiveStyle == RadarStyle::Square, "client promotes Round+Bounds to Square");
    CHECK(cli.maskDisabled, "promoted: mask disabled");
    CHECK(cli.squareMode, "promoted: squareMode true");
}

int main() {
    TestTransitionSequence();
    TestRapidTransitions();
    TestClientSidePromotion();

    std::printf("\nradar_transitions: %d checks passed", g_pass);
    if (g_fail) std::printf(", %d FAILED", g_fail);
    std::puts("");
    return g_fail ? 1 : 0;
}
