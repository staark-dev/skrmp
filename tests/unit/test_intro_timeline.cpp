// Timeline/transport tests for the urban intro (no GTA, no D3D device, no audio device).
#include "intro/intro_timeline.h"
#include "intro/intro_transport.h"
#include <cmath>
#include <cstdio>

using namespace skr::intro;

static int g_failed = 0, g_passed = 0;

static void Check(bool ok, const char* what, double got, double want) {
    if (ok) { ++g_passed; return; }
    ++g_failed;
    printf("  FAIL %s: got %.6f want %.6f\n", what, got, want);
}
static void Near(const char* what, double got, double want, double eps = 1e-3) {
    Check(std::fabs(got - want) <= eps, what, got, want);
}

static void TestTimeline() {
    // t = 0
    Layer bg = Background(0.0f);
    Near("bg u0 @0", bg.uv.u0, 0.5 - 0.5 / 1.055);
    Near("bg v0 @0", bg.uv.v0, 0.5 - 0.5 / 1.055);
    Near("bg u1-u0 @0", bg.uv.u1 - bg.uv.u0, 1.0 / 1.055);
    Near("fg alpha @0", Foreground(0.0f).alpha, 0.0);
    Near("fg x @0", Foreground(0.0f).rect.x, -60.0);
    Near("logo alpha @0", Logo(0.0f).alpha, 0.0);
    Near("logo x @0", Logo(0.0f).rect.x, 898.0);
    Near("logo y @0", Logo(0.0f).rect.y, 221.0);
    Near("footer alpha @0", Footer(0.0f).alpha, 0.0);
    Near("k @0", MasterFade(0.0f), 0.0);

    // t = 0.35
    Near("fg alpha @0.35", Foreground(0.35f).alpha, 0.0);
    Near("k @0.35", MasterFade(0.35f), 0.35 / 0.45);

    // t = 1.25
    Near("logo alpha @1.25", Logo(1.25f).alpha, 0.0);
    Near("fg alpha @1.25", Foreground(1.25f).alpha, 0.9 / 1.05);
    Near("k @1.25", MasterFade(1.25f), 1.0);

    // t = 1.9
    Near("subtitle alpha @1.9", Text(kTextLayers[0], 1.9f).alpha, 0.0);
    Near("logo alpha @1.9", Logo(1.9f).alpha, 0.65 / 0.70);
    Near("logo x @1.9", Logo(1.9f).rect.x, 850.0 + 48.0 * std::pow(1.0 - 0.65 / 0.8, 3.0));

    // t = 2.45
    Near("footer alpha @2.45", Footer(2.45f).alpha, 0.0);
    Near("logo x @2.45", Logo(2.45f).rect.x, 850.0);
    Near("logo y @2.45", Logo(2.45f).rect.y, 205.0);
    Near("prototype alpha @2.45", Text(kTextLayers[1], 2.45f).alpha, 0.5);
    Near("subtitle alpha @2.45", Text(kTextLayers[0], 2.45f).alpha, 0.55 / 0.60);

    // t = 3.2
    Near("fg x @3.2", Foreground(3.2f).rect.x, -18.0 + 10.0 * 3.2 / 9.0);
    Near("fg alpha @3.2", Foreground(3.2f).alpha, 1.0);
    Near("footer alpha @3.2", Footer(3.2f).alpha, 1.0);
    Near("subtitle alpha @3.2", Text(kTextLayers[0], 3.2f).alpha, 1.0);
    Near("prototype alpha @3.2", Text(kTextLayers[1], 3.2f).alpha, 1.0);
    Near("line1 alpha @3.2", Text(kTextLayers[2], 3.2f).alpha, 1.0);
    Near("line2 alpha @3.2", Text(kTextLayers[3], 3.2f).alpha, 0.5 / 0.6);
    Near("line2 alpha @3.3", Text(kTextLayers[3], 3.3f).alpha, 1.0);

    // t = 6, 8.25, 9
    Near("k @6", MasterFade(6.0f), 1.0);
    Near("k @8.25", MasterFade(8.25f), 1.0);
    Near("k @8.625", MasterFade(8.625f), 0.5);
    Near("k @9", MasterFade(9.0f), 0.0);
    Layer bg9 = Background(9.0f);
    Near("bg frame clamps at 269", BackgroundFrame(9.0f), 269.0);
    Near("bg u0 @9", bg9.uv.u0, 0.5 - 0.5 / 1.03 + 20.0 * std::sin(1.0) / 3840.0);
    Near("bg frame @1/30", BackgroundFrame(1.0f / 30.0f), 1.0);

    // Audio envelope
    Near("gain @0", AudioGain(0.0f), 0.0);
    Near("gain @0.4", AudioGain(0.4f), 0.35);
    Near("gain @4", AudioGain(4.0f), 0.70);
    Near("gain @8.55", AudioGain(8.55f), 0.35);
    Near("gain @9", AudioGain(9.0f), 0.0);

    // Loading progress
    Near("progress 0 chunks", LoadProgress(0, false), 0.0);
    Near("progress 70 chunks", LoadProgress(70, false), 0.4);
    Near("progress 140 chunks", LoadProgress(140, false), 0.8);
    Near("progress over 140 clamps", LoadProgress(200, false), 0.8);
    Near("progress connected", LoadProgress(140, true), 1.0);
    Near("bar width 30%", kBarRect.w, 0.30 * 1920.0);
    Check(kBarRect.x + kBarRect.w <= 1920.0f, "bar inside canvas", kBarRect.x + kBarRect.w, 1920);
    Check(kBarRect.x >= 1296.0f + 16.0f, "bar clear of line2", kBarRect.x, 1312);
    Check(kBarRect.y >= 1014.0f, "bar below line1", kBarRect.y, 1014);

    // Placement (pillar/letterbox, no stretch)
    Placement p = Place(1920, 1080);
    Near("place 1080p s", p.s, 1.0); Near("place 1080p ox", p.ox, 0.0);
    p = Place(800, 600);
    Near("place 800x600 s", p.s, 800.0 / 1920.0); Near("place 800x600 oy", p.oy, 75.0);
    p = Place(2560, 1080);
    Near("place ultrawide s", p.s, 1.0); Near("place ultrawide ox", p.ox, 320.0);
    p = Place(1280, 720);
    Near("place 720p s", p.s, 2.0 / 3.0); Near("place 720p ox", p.ox, 0.0);
}

static void TestTransport() {
    {   // audio absent: monotonic clock, large deltas are not clamped, ends at 9
        Transport tr; tr.Start(0.0);
        tr.SetWorldReady(true);
        Near("mono 0.5", tr.Update(0.5, -1), 0.5);
        Near("mono big delta", tr.Update(5.5, -1), 5.5);
        Near("mono end", tr.Update(10.0, -1), 9.0);
        Check(tr.GetState() == Transport::State::Finished, "finished at 9", 0, 1);
    }
    {   // audio clock drives time
        Transport tr; tr.Start(0.0);
        Near("audio 0.1", tr.Update(0.1, 0.08), 0.08);
        Near("audio 0.2", tr.Update(0.2, 0.19), 0.19);
        Check(tr.UsingAudioClock(), "uses audio clock", 0, 1);
    }
    {   // stalled audio falls back to the monotonic clock
        Transport tr; tr.Start(0.0);
        tr.Update(0.1, 0.1);
        Near("stall before threshold", tr.Update(0.5, 0.1), 0.1);
        Near("stall fallback", tr.Update(0.7, 0.1), 0.7);
        Check(!tr.UsingAudioClock(), "audio marked dead", 1, 0);
        Near("after fallback", tr.Update(1.0, 0.1), 1.0);
    }
    {   // pause / resume excludes the paused interval
        Transport tr; tr.Start(0.0);
        tr.Update(1.0, -1);
        tr.Pause();
        Near("paused holds", tr.Update(5.0, -1), 1.0);
        tr.Resume(5.0);
        Near("resumed", tr.Update(6.0, -1), 2.0);
    }
    {   // a key held before start never skips
        Transport tr; tr.Start(0.0);
        tr.Update(1.0, -1);
        Check(!tr.OnSkipKey(true), "held key ignored", 1, 0);
        tr.OnSkipKey(false);
        Check(tr.OnSkipKey(true), "skip after release", 0, 1);
        Check(tr.GetState() == Transport::State::Finished, "skip finishes", 0, 1);
    }
    {   // skip threshold 700 ms
        Transport tr; tr.Start(0.0);
        tr.OnSkipKey(false);
        tr.Update(0.69, -1);
        Check(!tr.OnSkipKey(true), "no skip before 0.7", 1, 0);
        tr.OnSkipKey(false);
        tr.Update(0.70, -1);
        Check(tr.OnSkipKey(true), "skip at 0.7", 0, 1);
    }
    {   // hold until the world is ready, then the final fade runs from kHoldAt
        Transport tr; tr.Start(0.0);
        Near("hold reaches 8.1", tr.Update(10.0, -1), 8.1);
        Near("hold keeps 8.1", tr.Update(20.0, -1), 8.1);
        Near("hold offset", tr.HoldOffset(), 11.9);
        Check(tr.GetState() == Transport::State::Playing, "still playing while held", 0, 1);
        tr.SetWorldReady(true);
        Near("released +0.5", tr.Update(20.5, -1), 8.6);
        Near("released clock", tr.Clock(), 20.5);
        Near("released end", tr.Update(21.5, -1), 9.0);
        Check(tr.GetState() == Transport::State::Finished, "finished after release", 0, 1);
    }
    {   // world ready before kHoldAt: no hold at all, 9 s total
        Transport tr; tr.Start(0.0);
        tr.Update(3.0, -1);
        tr.SetWorldReady(true);
        Near("no hold 8.5", tr.Update(8.5, -1), 8.5);
        Near("no hold offset", tr.HoldOffset(), 0.0);
        Near("no hold end", tr.Update(9.2, -1), 9.0);
    }
    {   // hold on the audio clock: intro time holds while audio keeps advancing
        Transport tr; tr.Start(0.0);
        tr.Update(8.0, 8.0);
        Near("audio hold", tr.Update(12.0, 12.0), 8.1);
        Near("audio hold offset", tr.HoldOffset(), 3.9);
    }
    // Audio envelope during the hold: full volume, fades on the intro time after release
    Near("gain held", AudioGain(30.0f, 8.1f), 0.70);
    Near("gain release +0.45", AudioGain(30.45f, 8.55f), 0.35);
    Near("gain release end", AudioGain(31.0f, 9.0f), 0.0);

    // FPS independence: the same wall time gives the same intro time at 5/30/60/144 FPS
    for (double fps : {5.0, 30.0, 60.0, 144.0}) {
        Transport tr; tr.Start(0.0);
        double t = 0.0;
        for (int i = 1; i <= static_cast<int>(fps * 4.0); ++i) t = tr.Update(i / fps, -1);
        char name[32];
        snprintf(name, sizeof(name), "fps %.0f -> 4 s", fps);
        Near(name, t, 4.0, 1e-6);
    }
}

int main() {
    TestTimeline();
    TestTransport();
    printf("SKRIntroTests: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
