// PHONE-001 M2 hotfix regression tests: phone-key edge filtering, open
// eligibility, and the controller lifecycle exactly as phone_input drives it
// (policy::KeyEdge -> policy::OnPhoneKeyEdge -> Controller). No Win32 or game
// memory; GameView stands in for the sampled GTA/SA-MP state.
#include "phone/phone_controller.h"
#include "phone/phone_policy.h"

#include <cmath>
#include <cstdio>

using namespace skr::phone;
using namespace skr::phone::policy;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { if (cond) ++g_pass; else { ++g_fail; std::printf("  FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

namespace {

constexpr uint32_t kLDown   = 0x00260001u;              // scancode 0x26, repeat count 1
constexpr uint32_t kLRepeat = kLDown | (1u << 30);      // previous-state bit set

GameView OnFoot() {
    GameView g;
    g.pedValid = true;
    g.alive = true;
    return g;
}

struct Sim {
    Controller c;
    KeyEdge edge;
    GameView game = OnFoot();
    uint64_t t = 10000;
    int evaluations = 0;

    // One WM_KEYDOWN for L, routed the way phone_input::ProcessMessage does.
    // Returns false when the edge filter rejected the message.
    bool Key(uint32_t lParam, KeyResult* out = nullptr) {
        if (!edge.Down(lParam, t)) return false;
        const KeyResult r = OnPhoneKeyEdge(c, t, [&] { ++evaluations; return Evaluate(game); });
        if (out) *out = r;
        return true;
    }
    KeyResult Press() { KeyResult r = KeyResult::Ignored; Key(kLDown, &r); return r; }
    void Release() { edge.Up(); }
    KeyResult Tap() { const KeyResult r = Press(); Release(); return r; }

    // Frames at ~60 fps; the renderer is never involved.
    void Run(uint32_t ms) {
        for (uint32_t e = 0; e < ms; e += 16) { t += 16; c.Update(t); }
    }
};

bool Settled(State s) { return s == State::Hidden || s == State::Home || s == State::AppActive; }

void TestEligibility() {
    // Regression for the M2 root cause: standing on foot is NOT "in a vehicle".
    CHECK(Evaluate(OnFoot()) == Block::None, "standing on foot may open");
    GameView g = OnFoot();
    g.inVehicle = true; g.isDriver = true;
    CHECK(Evaluate(g) == Block::Driver, "driver is blocked");
    g.isDriver = false;
    CHECK(Evaluate(g) == Block::None, "passenger may open");
    g = OnFoot(); g.alive = false;
    CHECK(Evaluate(g) == Block::Dead, "dead is blocked");
    CHECK(Evaluate(GameView{}) == Block::NoPed, "missing ped is blocked");
    g = OnFoot(); g.chatOpen = true;
    CHECK(Evaluate(g) == Block::ChatOpen, "chat blocks");
    g = OnFoot(); g.dialogOpen = true;
    CHECK(Evaluate(g) == Block::DialogOpen, "dialog blocks");
    // A failed SA-MP read reports chat/dialog closed: the phone stays usable.
    g = OnFoot(); g.chatOpen = false; g.dialogOpen = false;
    CHECK(Evaluate(g) == Block::None, "SA-MP read failure does not block");
    for (int b = 0; b <= static_cast<int>(Block::DialogOpen); ++b)
        CHECK(BlockName(static_cast<Block>(b))[0] != '?', "every block reason has a name");
}

void TestTwentyCycles() {
    Sim s;
    for (int i = 0; i < 20; ++i) {
        CHECK(s.Tap() == KeyResult::Opened, "cycle: L opens");
        CHECK(s.c.OwnsInput(), "cycle: phone owns input while opening");
        s.Run(400);
        CHECK(s.c.GetState() == State::Home, "cycle: opening settles on HOME");
        CHECK(ShouldRender(s.c), "cycle: visible phone renders");
        CHECK(s.Tap() == KeyResult::Closed, "cycle: L closes");
        CHECK(!s.c.OwnsInput(), "cycle: input returns to game at close start");
        s.Run(400);
        CHECK(s.c.GetState() == State::Hidden, "cycle: closing settles on HIDDEN");
        CHECK(!ShouldRender(s.c), "cycle: hidden phone stops rendering (hits dropped)");
    }
}

void TestRapidPresses() {
    Sim s;
    KeyResult last = KeyResult::Ignored;
    for (int i = 0; i < 11; ++i) { last = s.Tap(); s.Run(32); }
    CHECK(last == KeyResult::Opened, "odd number of rapid presses ends opening");
    s.Run(600);
    CHECK(s.c.GetState() == State::Home, "rapid presses settle open");
    for (int i = 0; i < 10; ++i) { s.Tap(); s.Run(32); }
    s.Run(600);
    CHECK(s.c.GetState() == State::Home, "even number of rapid presses returns to the same state");
    s.Tap(); s.Run(600);
    CHECK(s.c.GetState() == State::Hidden, "phone still closes after rapid presses");
}

void TestAutoRepeat() {
    Sim s;
    CHECK(s.Press() == KeyResult::Opened, "first down opens");
    int accepted = 0;
    for (int i = 0; i < 40; ++i) { s.t += 33; if (s.Key(kLRepeat)) ++accepted; }
    CHECK(accepted == 0, "auto-repeat downs never toggle");
    s.Run(400);
    CHECK(s.c.GetState() == State::Home, "held L leaves the phone open");
    s.Release();
    CHECK(s.Tap() == KeyResult::Closed, "next real press closes");
}

void TestDuplicateMessage() {
    Sim s;
    KeyResult r = KeyResult::Ignored;
    CHECK(s.Key(kLDown, &r) && r == KeyResult::Opened, "first delivery opens");
    CHECK(!s.Key(kLDown), "same press delivered twice is rejected");
    s.t += 10;
    CHECK(!s.Key(kLDown), "duplicate within the latch window is rejected");
    CHECK(s.c.GetState() == State::Opening, "duplicate did not close the phone");
    // Key-up swallowed by another window procedure: the latch must not wedge.
    s.t += KeyEdge::kRelatchMs;
    CHECK(s.Key(kLDown, &r) && r == KeyResult::Closed, "fresh press after lost key-up is accepted");
}

void TestInterruptedTransitions() {
    Sim s;
    s.Tap();
    s.Run(96);
    const float mid = s.c.Slide();
    CHECK(mid > 0.05f && mid < 0.95f, "mid-open position");
    CHECK(s.Tap() == KeyResult::Closed, "L during OPENING closes");
    CHECK(s.c.GetState() == State::Closing, "OPENING -> CLOSING");
    CHECK(std::fabs(s.c.Slide() - mid) < 1e-4f, "reverse keeps on-screen position");
    s.Run(48);
    const float back = s.c.Slide();
    CHECK(s.Tap() == KeyResult::Opened, "L during CLOSING reopens");
    CHECK(s.c.GetState() == State::Opening, "CLOSING -> OPENING");
    CHECK(std::fabs(s.c.Slide() - back) < 1e-4f, "re-open keeps on-screen position");
    s.Run(500);
    CHECK(s.c.GetState() == State::Home, "interrupted sequence settles open");
    CHECK(s.evaluations == 2, "eligibility sampled only for opening presses");
}

void TestChatAndDialog() {
    Sim s;
    s.Tap(); s.Run(400);
    // T/F6 while open: phone_input closes the phone, then SA-MP opens chat.
    CHECK(s.c.Close(s.t), "chat key closes the phone");
    s.game.chatOpen = true;
    s.Run(400);
    CHECK(s.Tap() == KeyResult::Blocked, "L typed in chat does not open");
    CHECK(s.c.GetState() == State::Hidden, "blocked press leaves phone hidden");
    s.game.chatOpen = false;
    CHECK(s.Tap() == KeyResult::Opened, "phone opens after chat closes");
    s.Run(400);
    s.Tap(); s.Run(400);
    s.game.dialogOpen = true;
    CHECK(s.Tap() == KeyResult::Blocked, "dialog blocks opening");
    s.game.dialogOpen = false;
    CHECK(s.Tap() == KeyResult::Opened, "phone opens after dialog closes");
    // Blocked press whose key-up was eaten by chat still cannot wedge the key.
    s.Run(400); s.Tap(); s.Run(400);
    s.game.chatOpen = true;
    CHECK(s.Press() == KeyResult::Blocked, "blocked press (no key-up follows)");
    s.game.chatOpen = false;
    s.t += KeyEdge::kRelatchMs;
    CHECK(s.Press() == KeyResult::Opened, "next press after lost key-up opens");
}

void TestPauseSuspend() {
    Sim s;
    s.Tap(); s.Run(400);
    CHECK(s.c.OpenApp(AppId::Messages, s.t), "open Messages");
    s.Run(400);
    CHECK(s.c.GetState() == State::AppActive, "Messages active");
    s.c.Suspend();                          // pause menu opened
    CHECK(s.c.GetState() == State::Suspended, "pause suspends");
    CHECK(!s.c.OwnsInput(), "suspended phone releases input");
    s.Run(5000);
    CHECK(s.c.GetState() == State::Suspended, "suspension does not decay by itself");
    CHECK(s.Tap() == KeyResult::Opened, "L after pause reopens");
    s.Run(400);
    CHECK(s.c.GetState() == State::AppActive && s.c.Current().app == AppId::Messages,
          "reopen restores the suspended screen");
    // Suspended while closing still recovers.
    s.Tap(); s.Run(64);
    s.c.Suspend();
    CHECK(s.c.GetState() == State::Suspended, "pause during CLOSING suspends");
    CHECK(s.Tap() == KeyResult::Opened, "and L reopens");
}

void TestFocusLoss() {
    Sim s;
    s.Press(); s.Run(400);                  // opened, L still held
    CHECK(s.edge.held, "key latched while held");
    // WM_KILLFOCUS: the key-up will be delivered to another window.
    s.edge.Reset();
    CHECK(!s.edge.held, "focus loss clears the latch");
    s.c.Suspend();
    s.t += 30000;                           // 30 s on the desktop
    CHECK(s.c.Resume(s.t), "focus regained resumes");
    s.t += 5000;                            // one huge first frame
    s.c.Update(s.t);
    CHECK(Settled(s.c.GetState()) && s.c.GetState() != State::Hidden, "large delta finishes cleanly");
    CHECK(s.c.Slide() == 1.0f, "slide clamped to fully open");
    CHECK(s.Tap() == KeyResult::Closed, "key works after Alt+Tab");
    // Clock going backwards (tick wrap / skew) never advances or corrupts state.
    s.Run(400); s.Tap();
    s.c.Update(s.t - 1000);
    CHECK(s.c.GetState() == State::Opening, "backwards clock is a zero-length frame");
    s.Run(500);
    CHECK(s.c.GetState() == State::Home, "and the next frames complete the open");
}

void TestRendererAbsent() {
    // D3D9 reset / lost device: Render() is skipped but Tick still updates.
    Sim s;
    s.Tap();
    s.Run(0);
    CHECK(s.c.GetState() == State::Opening, "opening with no frames yet");
    s.Run(800);                             // frames with no draw at all
    CHECK(s.c.GetState() == State::Home, "open completes without the renderer");
    s.Tap();
    s.Run(800);
    CHECK(s.c.GetState() == State::Hidden, "close completes without the renderer");
    CHECK(s.Tap() == KeyResult::Opened, "and the phone reopens");
}

void TestDriverPassenger() {
    Sim s;
    s.game.inVehicle = true; s.game.isDriver = false;
    CHECK(s.Tap() == KeyResult::Opened, "passenger opens");
    s.Run(400);
    CHECK(WhileOpen(s.game) == Running::Keep, "passenger keeps the phone");
    s.game.isDriver = true;                 // shuffled into the driver seat
    CHECK(WhileOpen(s.game) == Running::Close, "driver seat closes the phone");
    s.c.Close(s.t); s.Run(400);
    CHECK(s.Tap() == KeyResult::Blocked, "driver cannot reopen");
    s.game = OnFoot();                      // exited: m_pVehicle may still be set, bInVehicle is not
    CHECK(s.Tap() == KeyResult::Opened, "on foot after driving opens");
}

void TestDeathAndDisconnect() {
    Sim s;
    s.Tap(); s.Run(400);
    s.game.alive = false;
    CHECK(WhileOpen(s.game) == Running::Reset, "death resets");
    s.c.Reset();
    CHECK(s.c.GetState() == State::Hidden && !s.c.OwnsInput(), "reset hides and releases input");
    CHECK(s.Tap() == KeyResult::Blocked, "dead player cannot open");
    s.game = GameView{};                    // ped destroyed (disconnect / class selection)
    CHECK(WhileOpen(s.game) == Running::Reset, "missing ped resets");
    CHECK(s.Tap() == KeyResult::Blocked, "no ped cannot open");
    s.game = OnFoot();                      // respawned / reconnected
    CHECK(s.Tap() == KeyResult::Opened, "phone opens after respawn");
    s.Run(400);
    CHECK(s.c.GetState() == State::Home && s.c.Current().app == AppId::Home, "fresh phone after reset");
}

void TestNoStaleRender() {
    Sim s;
    s.Tap(); s.Run(400);
    s.c.OpenApp(AppId::Contacts, s.t); s.Run(400);
    s.Tap();
    CHECK(ShouldRender(s.c), "closing slide still renders");
    CHECK(!s.c.AcceptsClicks(), "closing phone ignores clicks");
    s.Run(400);
    CHECK(!ShouldRender(s.c), "closed phone renders nothing, so hit regions are cleared");
    s.c.PushNotification(s.t);
    CHECK(ShouldRender(s.c) && !s.c.AcceptsClicks(), "banner renders without click targets");
}

} // namespace

int main() {
    TestEligibility();
    TestTwentyCycles();
    TestRapidPresses();
    TestAutoRepeat();
    TestDuplicateMessage();
    TestInterruptedTransitions();
    TestChatAndDialog();
    TestPauseSuspend();
    TestFocusLoss();
    TestRendererAbsent();
    TestDriverPassenger();
    TestDeathAndDisconnect();
    TestNoStaleRender();
    std::printf("\nPhone M2 tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
