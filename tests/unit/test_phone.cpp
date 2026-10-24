// PHONE-001 M1 — SKR Street Phone layout + state/animation controller.
// Pure logic, links client/phone/phone_controller.cpp. Exit 0 = pass, 1 = fail.

#include <cmath>
#include <cstdio>

#include "phone/phone_controller.h"
#include "phone/phone_layout.h"
#include "phone/phone_model.h"

using namespace skr::phone;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { if (cond) ++g_pass; else { ++g_fail; std::printf("  FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

static bool Near(float a, float b, float eps = 0.002f) { return std::fabs(a - b) <= eps; }

static bool Inside(const Rect& in, const Rect& out) {
    return in.x0 >= out.x0 && in.y0 >= out.y0 && in.x1 <= out.x1 && in.y1 <= out.y1;
}

static void TestLayout() {
    std::printf("layout\n");
    const float sizes[][2] = {{1360, 768}, {1600, 900}, {1920, 1080}, {2560, 1440}, {2560, 1080}, {3440, 1440}, {5120, 1440}, {1280, 720},
                              {1024, 768}, {800, 600}, {2560, 1600}};
    for (const auto& s : sizes)
        for (float scale : {0.5f, 0.75f, 1.0f, 1.25f, 1.35f, 2.0f}) {
            LayoutInput in;
            in.screenW = s[0]; in.screenH = s[1]; in.uiScale = scale;
            const Layout L = ComputeLayout(in);
            const Rect screen{0, 0, s[0], s[1]};
            CHECK(Inside(L.body, screen), "body inside the backbuffer");
            CHECK(L.body.y0 >= s[1] * kReservedTop - 0.5f, "body below the reserved HUD band");
            CHECK(Inside(L.screen, L.body), "screen inside body");
            CHECK(Inside(L.content, L.screen) && Inside(L.statusBar, L.screen) && Inside(L.navBar, L.screen),
                  "status/content/nav inside screen");
            CHECK(Near(L.statusBar.y1, L.content.y0, 0.01f) && Near(L.content.y1, L.navBar.y0, 0.01f),
                  "status, content and nav are contiguous");
            CHECK(Inside(L.island, L.statusBar), "camera island inside status bar");
            CHECK(Inside(L.homeIndicator, L.navBar), "home indicator inside nav bar");
            CHECK(L.body.x1 > s[0] * 0.5f, "phone sits in the right half");
            const float aspect = L.body.H() / L.body.W();
            CHECK(aspect > 2.0f && aspect < 2.1f, "device aspect ~ 2.04");
            CHECK(L.screenRadius < L.bodyRadius && L.screenRadius >= 4.0f, "screen radius nests in body radius");
            const float regionW = std::fmin(s[0], s[1] * kMaxAspect);
            CHECK(L.body.x1 <= (s[0] + regionW) * 0.5f, "anchored inside the centred 21:9 region");
        }

    LayoutInput a; a.screenW = 1360; a.screenH = 768;
    const Layout la = ComputeLayout(a);
    CHECK(Near(la.u, 1.0f) && Near(la.body.H(), 400.0f, 0.6f) && Near(la.body.W(), 196.0f, 0.6f), "1360x768 reference size");
    LayoutInput b; b.screenW = 1920; b.screenH = 1080;
    const Layout lb = ComputeLayout(b);
    CHECK(Near(lb.body.H() / 1080.0f, la.body.H() / 768.0f, 0.002f), "same screen fraction at 1080p");
    b.uiScale = 9.0f;
    CHECK(Near(ComputeLayout(b).u, ComputeLayout({1920, 1080, kMaxUiScale}).u), "UI scale clamped high");
    b.uiScale = 0.1f;
    CHECK(Near(ComputeLayout(b).u, ComputeLayout({1920, 1080, kMinUiScale}).u), "UI scale clamped low");

    LayoutInput uw; uw.screenW = 5120; uw.screenH = 1440;
    const Layout lu = ComputeLayout(uw);
    CHECK(lu.body.x1 < 5120 * 0.85f, "32:9: phone pulled toward the centre");

    LayoutInput hid = a; hid.slide = 0.0f;
    CHECK(ComputeLayout(hid).body.y0 >= 768.0f, "slide 0: fully below the screen");
    LayoutInput half = a; half.slide = 0.5f;
    const Layout lh = ComputeLayout(half);
    CHECK(lh.body.y0 > la.body.y0 && lh.body.y0 < 768.0f, "slide 0.5: between rest and hidden");
    CHECK(Near(lh.body.H(), la.body.H(), 0.01f) && Near(lh.body.x0, la.body.x0, 0.01f), "slide only translates vertically");

    LayoutInput tiny; tiny.screenW = 320; tiny.screenH = 240;
    CHECK(!ComputeLayout(tiny).fits, "unusably small screen reports !fits");
}

static void TestController() {
    std::printf("controller\n");
    Controller c;
    c.Update(100);
    CHECK(c.GetState() == State::Hidden && !c.Visible() && !c.OwnsInput() && !c.Animating(), "starts hidden, idle");
    CHECK(!c.Back(100) && !c.OpenApp(AppId::Gps, 100) && !c.Close(100), "hidden ignores navigation");

    // Frame-rate independence: 30 fps vs 144 fps reach the same slide at the same time.
    Controller a, b;
    a.Open(0); b.Open(0);
    for (uint64_t t = 0; t <= 140; t += 33) a.Update(t);
    for (uint64_t t = 0; t <= 140; t += 7) b.Update(t);
    a.Update(140); b.Update(140);
    CHECK(Near(a.Slide(), b.Slide()), "slide identical at 30 and 144 fps");
    CHECK(a.GetState() == State::Opening && a.Slide() > 0.3f && a.Slide() < 1.0f, "mid-open state");
    CHECK(a.OwnsInput() && !a.AcceptsClicks(), "input owned while opening, clicks not accepted yet");

    // Completion.
    a.Update(1000);
    CHECK(a.GetState() == State::Home && Near(a.Slide(), 1.0f) && Near(a.Alpha(), 1.0f), "opened to Home");
    CHECK(a.AcceptsClicks() && !a.Animating(), "Home accepts clicks and is idle");

    // Interrupted open -> close reverses with no jump.
    Controller r;
    r.Open(0); r.Update(120);
    const float before = r.Slide();
    r.Close(120); r.Update(120);
    CHECK(Near(r.Slide(), before), "close mid-open continues from the same position");
    r.Update(130);
    CHECK(r.Slide() < before, "and moves back down");
    r.Open(130); r.Update(130);
    CHECK(r.GetState() == State::Opening && r.Slide() < before, "re-open mid-close reverses again");
    r.Update(5000);
    CHECK(r.GetState() == State::Home, "settles open");
    r.Close(5000); r.Update(5000 + 220);
    CHECK(r.GetState() == State::Hidden && !r.OwnsInput(), "close completes to Hidden, input released");

    // App navigation.
    Controller n;
    n.Open(0); n.Update(1000);
    CHECK(n.OpenApp(AppId::Messages, 1000) && n.GetState() == State::AppTransition, "open app starts zoom");
    CHECK(n.Kind() == TransitionKind::Zoom && n.Forward(), "zoom forward");
    CHECK(!n.OpenApp(AppId::Gps, 1001) && !n.Back(1001) && !n.AcceptsClicks(), "no competing transitions");
    n.Update(1000 + 240);
    CHECK(n.GetState() == State::AppActive && n.Current().app == AppId::Messages, "app active");
    CHECK(n.OpenPage(1, 1240) && n.Kind() == TransitionKind::Slide, "page slide");
    n.Update(2000);
    CHECK(n.Current().page == 1, "conversation page");
    CHECK(n.Back(2000) && n.Kind() == TransitionKind::Slide && !n.Forward(), "back slides to the list");
    n.Update(3000);
    CHECK(n.Current().page == 0 && n.GetState() == State::AppActive, "list page");
    CHECK(n.Back(3000) && n.Kind() == TransitionKind::Zoom && !n.Forward(), "back zooms to Home");
    n.Update(4000);
    CHECK(n.GetState() == State::Home, "home");
    CHECK(n.Back(4000) && n.GetState() == State::Closing, "back on Home closes");

    // Close during a transition completes it instantly.
    Controller t;
    t.Open(0); t.Update(1000); t.OpenApp(AppId::Gps, 1000); t.Update(1100);
    CHECK(t.Close(1100) && t.Kind() == TransitionKind::None && t.GetState() == State::Closing, "close mid-transition");

    // Suspend / resume (focus loss, pause menu, chat) keeps the screen.
    Controller s;
    s.Open(0); s.Update(1000); s.OpenApp(AppId::Contacts, 1000); s.Update(2000);
    s.Suspend();
    CHECK(s.GetState() == State::Suspended && !s.Visible() && !s.OwnsInput(), "suspended hides and releases input");
    s.Update(3000);
    CHECK(s.GetState() == State::Suspended, "suspended does not advance");
    CHECK(s.Resume(3000) && s.GetState() == State::Opening, "resume re-opens");
    s.Update(4000);
    CHECK(s.GetState() == State::AppActive && s.Current().app == AppId::Contacts, "resume returns to the same app");

    // Reset (death / disconnect) clears everything.
    s.BeginIncomingCall(4000);
    s.Reset();
    CHECK(s.GetState() == State::Hidden && s.Current().app == AppId::Home && !s.CallIncoming(), "reset clears state");

    // Incoming call opens a hidden phone; ESC does not dismiss the call screen.
    Controller k;
    k.BeginIncomingCall(500);
    CHECK(k.GetState() == State::Opening && k.CallIncoming(), "call opens the phone");
    k.Update(1500);
    CHECK(!k.Back(1500), "back ignored while ringing");
    const float p0 = k.RingPhase(1500), p1 = k.RingPhase(1500 + k.Timing().ringPeriodMs);
    CHECK(Near(p0, p1), "ring phase is periodic and deterministic");
    k.EndCall();
    CHECK(!k.CallIncoming(), "call ended");

    // Press feedback decays and stops animating.
    Controller pr;
    pr.Open(0); pr.Update(1000);
    pr.Press(7, 1000);
    CHECK(Near(pr.PressAmount(7, 1050), 1.0f) && Near(pr.PressAmount(8, 1050), 0.0f), "press held on its target only");
    pr.Release(1100);
    CHECK(pr.PressAmount(7, 1170) > 0.0f && pr.PressAmount(7, 1170) < 1.0f, "press decays after release");
    pr.Update(1100 + pr.Timing().pressMs);
    CHECK(!pr.Animating(), "press feedback ends");

    // Notification timing.
    Controller nt;
    nt.PushNotification(1000);
    CHECK(Near(nt.NotifyAmount(1000), 0.0f) && Near(nt.NotifyAmount(2000), 1.0f), "banner slides in, holds");
    const Durations d;
    CHECK(Near(nt.NotifyAmount(1000 + d.notifyInMs + d.notifyHoldMs + d.notifyOutMs + 1), 0.0f), "banner gone after out");

    // Easing.
    CHECK(Near(EaseOutCubic(0), 0) && Near(EaseOutCubic(1), 1) && EaseOutCubic(0.5f) > 0.5f, "ease out cubic");
    CHECK(Near(EaseOutCubic(-1), 0) && Near(EaseOutCubic(2), 1), "ease clamps");
}

static void TestModel() {
    std::printf("model\n");
    char small[6];
    CopyText(small, "Grove Street");
    CHECK(std::strlen(small) == 5 && small[5] == 0, "CopyText truncates and terminates");
    CopyText(small, nullptr);
    CHECK(small[0] == 0, "CopyText null -> empty");
    Model m;
    CHECK(!m.previewData && m.contactCount == 0 && m.convo.count == 0, "empty model by default");
    CHECK(sizeof(Message::text) == 121, "SMS buffer matches gamemode SKR_PHONE_SMS_MAX_LEN 120");
    CHECK(sizeof(Model) < 32 * 1024, "model stays small and fixed-size");
}

int main() {
    TestLayout();
    TestController();
    TestModel();
    std::printf("\nPhone tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
