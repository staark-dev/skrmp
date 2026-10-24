// PHONE-001 M3/M5.9 regression tests: CJ's physical phone (body::Coordinator)
// driven exactly as phone_input drives it — controller + KeyEdge + policy +
// BodyRequest -> Coordinator — against a mock that models the lifecycle of
// GTA's CTaskComplexUseMobilePhone as read from the 1.0 US binary:
// phone_in (model added at 0.85 s, or earlier with browsing pose) ->
// phone_talk -> [quit] -> phone_out (model removed ~1.566 s) -> task ends;
// IMMEDIATE abort clears at once.
// M5.9: browsing pose is now the default (freeze phone_in at 0.65 s, model
// attached early via CPed::AddWeaponModel).  Legacy ear mode: HoldPose=0.
#include "phone/phone_body.h"
#include "phone/phone_controller.h"
#include "phone/phone_policy.h"

#include <chrono>
#include <cstdio>

using namespace skr::phone;
using namespace skr::phone::policy;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { if (cond) ++g_pass; else { ++g_fail; std::printf("  FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

namespace {

constexpr uint32_t kLDown = 0x00260001u;

struct MockGta final : body::Backend {
    enum class Sub { None, In, Talk, Out };
    uintptr_t ped = 0x1000;
    uintptr_t slot = 0;              // partial-anim secondary slot
    bool ours = false;               // slot holds a task Start() created
    Sub sub = Sub::None;
    int subMs = 0;
    bool quit = false, holding = false, prop = false, earlyProp = false;
    uintptr_t nextTask = 0x5000;
    // Bookkeeping / violations.
    int calls = 0, starts = 0, quits = 0, aborts = 0, holdOn = 0, holdOff = 0;
    int maxLive = 0;
    bool staleCall = false, wrongTask = false, doubleStart = false;

    void Touch(uintptr_t p) { ++calls; if (p != ped) staleCall = true; }

    uintptr_t Ped() override { ++calls; return ped; }
    uintptr_t SlotTask(uintptr_t p) override { Touch(p); return slot; }
    uintptr_t Start(uintptr_t p) override {
        Touch(p);
        if (slot) { doubleStart = true; return 0; }
        slot = nextTask; nextTask += 0x40;
        ours = true; sub = Sub::In; subMs = 0; quit = false; prop = false; holding = false;
        ++starts;
        if (maxLive < 1) maxLive = 1;
        return slot;
    }
    void Quit(uintptr_t p, uintptr_t t) override {
        Touch(p);
        if (t != slot || !ours) { wrongTask = true; return; }
        quit = true; ++quits;
    }
    bool Abort(uintptr_t p, uintptr_t t) override {
        Touch(p);
        if (t != slot || !ours) { wrongTask = true; return false; }
        Clear(); ++aborts;
        return true;
    }
    void Hold(uintptr_t p, bool on, float atSec) override {
        Touch(p);
        holding = on;
        on ? ++holdOn : ++holdOff;
        // Simulate early model attachment (M5.9 browsing pose): when the freeze
        // point is below GTA's natural 0.85 s and the animation has reached it,
        // the backend calls CPed::AddWeaponModel(330).
        if (on && atSec < 0.85f && sub == Sub::In &&
            subMs >= static_cast<int>(atSec * 1000.0f) && !prop) {
            prop = true;
            earlyProp = true;
        }
    }
    void Clear() { slot = 0; ours = false; sub = Sub::None; prop = false; earlyProp = false; quit = false; holding = false; }

    // One game frame of GTA's task processing.
    void Advance(int ms) {
        if (!slot || !ours) return;
        subMs += ms;
        switch (sub) {
            case Sub::In:
                if (subMs >= 850) prop = true;
                if (!holding && subMs >= 1200) { sub = quit ? Sub::Out : Sub::Talk; subMs = 0; }
                break;
            case Sub::Talk:
                if (quit) { sub = Sub::Out; subMs = 0; }
                break;
            case Sub::Out:
                if (subMs >= 1566) prop = false;
                if (subMs >= 1800) Clear();
                break;
            case Sub::None:
                break;
        }
    }
};

GameView OnFoot() { GameView g; g.pedValid = true; g.alive = true; return g; }

struct Sim {
    MockGta gta;
    Controller c;
    KeyEdge edge;
    body::Coordinator body{gta};
    GameView game = OnFoot();
    uint64_t t = 10000;

    KeyResult Tap() {
        KeyResult r = KeyResult::Ignored;
        if (edge.Down(kLDown, t)) r = OnPhoneKeyEdge(c, t, [&] { return Evaluate(game); });
        edge.Up();
        return r;
    }
    // One frame in the order phone_input::Tick runs it.
    void Frame() {
        t += 16;
        c.Update(t);
        if (c.GetState() != State::Hidden) {
            switch (WhileOpen(game)) {
                case Running::Reset: c.Reset(); break;
                case Running::Close: if (c.GetState() != State::Closing) c.Close(t); break;
                case Running::Keep: break;
            }
        }
        const bool needPed = c.GetState() != State::Hidden || body.Owns();
        body.Update(needPed ? BodyRequest(c, game) : body::Request{});
        gta.Advance(16);
        if (gta.slot && gta.maxLive < 1) gta.maxLive = 1;
    }
    void Run(int ms) { for (int e = 0; e < ms; e += 16) Frame(); }
    bool Clean() const { return !gta.slot && !gta.prop && !body.Owns(); }
    bool NoViolations() const { return !gta.staleCall && !gta.wrongTask && !gta.doubleStart; }
};

void TestOpenCloseOnce() {
    Sim s;
    CHECK(s.Tap() == KeyResult::Opened, "L opens");
    s.Frame();
    CHECK(s.gta.starts == 1, "opening triggers exactly one phone task");
    CHECK(s.body.GetPhase() == body::Phase::Active, "body active while opening");
    s.Run(1000);
    CHECK(s.gta.prop, "phone model in hand (browsing pose)");
    CHECK(s.gta.earlyProp, "model attached early (below 0.85 s)");
    CHECK(s.gta.holding, "phone_in frozen at browsing height");
    CHECK(s.gta.sub == MockGta::Sub::In, "still in phone_in (not phone_talk)");
    CHECK(s.gta.starts == 1, "holding never restarts the task");
    CHECK(s.Tap() == KeyResult::Closed, "L closes");
    s.Frame();
    CHECK(s.gta.quits == 1, "closing triggers exactly one phone_out");
    CHECK(s.body.GetPhase() == body::Phase::Quitting, "body quitting while phone_out plays");
    s.Run(3000);
    CHECK(s.Clean(), "task ended, model removed, nothing owned");
    CHECK(s.body.GetPhase() == body::Phase::Idle, "body idle after close");
    CHECK(s.gta.aborts == 0, "normal close never hard-aborts");
    CHECK(s.NoViolations(), "no stale/wrong/double calls");
}

void TestTwentyCycles() {
    Sim s;
    for (int i = 0; i < 20; ++i) {
        s.Tap(); s.Run(1500);
        CHECK(s.gta.prop && s.gta.slot, "cycle: phone in hand");
        s.Tap(); s.Run(2500);
        CHECK(s.Clean(), "cycle: nothing left after close");
    }
    CHECK(s.gta.starts == 20 && s.gta.quits == 20, "20 starts, 20 quits");
    CHECK(s.gta.maxLive <= 1, "never more than one phone task");
    CHECK(s.NoViolations(), "no violations over 20 cycles");
}

void TestRapidToggle() {
    Sim s;
    for (int i = 0; i < 25; ++i) { s.Tap(); s.Run(32); }
    CHECK(s.gta.starts <= 2, "rapid toggling never queues tasks");
    CHECK(s.gta.maxLive <= 1, "one task at most during rapid toggling");
    s.Run(4000);
    // 25 taps: ends open. Body ends holding the phone exactly once.
    CHECK(s.c.Visible(), "odd tap count leaves UI open");
    CHECK(s.gta.slot && s.gta.prop, "settles holding one phone");
    s.Tap(); s.Run(4000);
    CHECK(s.Clean(), "and closes cleanly");
    CHECK(s.NoViolations(), "no violations under rapid toggling");
}

void TestInterrupted() {
    Sim s;
    s.Tap(); s.Run(160);                          // mid phone_in, before the model appears
    CHECK(!s.gta.prop, "model not yet in hand at 160 ms");
    s.Tap(); s.Run(16);
    CHECK(s.gta.quits == 1, "close during OPENING quits");
    s.Run(4000);
    CHECK(s.Clean(), "interrupted open ends clean");
    // Close, then reopen while phone_out is still playing.
    s.Tap(); s.Run(1500);
    s.Tap(); s.Run(400);                          // phone_out running
    CHECK(s.gta.slot != 0, "phone_out still playing");
    const int startsBefore = s.gta.starts;
    s.Tap(); s.Run(16);
    CHECK(s.c.OwnsInput(), "UI reopens immediately");
    CHECK(s.gta.starts == startsBefore, "no second task while phone_out plays");
    CHECK(s.body.GetPhase() == body::Phase::Quitting, "body waits for phone_out");
    s.Run(3000);
    CHECK(s.gta.starts == startsBefore + 1, "exactly one restart after phone_out ends");
    CHECK(s.gta.prop, "phone back in hand");
    CHECK(s.NoViolations(), "no violations across interruptions");
}

void TestDeath() {
    Sim s;
    s.Tap(); s.Run(1500);
    s.game.alive = false;
    s.Frame();
    CHECK(s.gta.aborts == 1, "death hard-aborts the phone task");
    CHECK(s.Clean(), "death removes model and task immediately");
    CHECK(s.c.GetState() == State::Hidden, "UI reset on death");
    s.Run(1000);
    CHECK(s.gta.starts == 1, "no restart while dead");
    // GTA flushing the slot itself on death is also clean.
    Sim f;
    f.Tap(); f.Run(1500);
    f.gta.Clear();                                // engine flush (death event)
    f.game.alive = false;
    f.Frame();
    CHECK(f.gta.aborts == 0 && f.Clean(), "engine-flushed task is forgotten, not touched");
    CHECK(f.NoViolations() && s.NoViolations(), "no violations on death");
}

void TestDisconnectAndRespawn() {
    Sim s;
    s.Tap(); s.Run(1500);
    s.gta.ped = 0; s.gta.Clear();                 // ped destroyed (disconnect / server change)
    s.game = GameView{};
    s.Run(100);
    CHECK(!s.body.Owns(), "no reference kept after the ped is gone");
    CHECK(s.c.GetState() == State::Hidden, "UI reset without a ped");
    CHECK(!s.gta.staleCall, "no call with the destroyed ped");
    s.gta.ped = 0x2000; s.game = OnFoot();        // respawned ped
    s.Tap(); s.Run(1500);
    CHECK(s.gta.starts == 2 && s.gta.slot && s.gta.prop, "fresh task on the new ped");
    // Ped swapped while the phone is open (SA-MP recreating the local ped).
    s.gta.ped = 0x3000; s.gta.Clear();
    s.Run(64);
    CHECK(!s.body.Owns() && !s.gta.staleCall, "swapped ped: old task forgotten, never touched");
    CHECK(s.gta.starts == 2, "no restart inside the same open session");
    // Module shutdown with the phone out releases everything.
    Sim d;
    d.Tap(); d.Run(1500);
    d.body.Release(true);
    CHECK(d.gta.aborts == 1 && d.Clean(), "shutdown aborts and cleans up");
    CHECK(d.NoViolations(), "no violations on shutdown");
}

void TestDriverAndPassenger() {
    Sim s;
    s.game.inVehicle = true; s.game.isDriver = true;
    CHECK(s.Tap() == KeyResult::Blocked, "driver cannot open");
    s.Run(500);
    CHECK(s.gta.calls == 0, "blocked driver costs no game calls");
    s.game.isDriver = false;                      // passenger
    CHECK(s.Tap() == KeyResult::Opened, "passenger opens the UI");
    s.Run(1500);
    CHECK(s.c.OwnsInput(), "passenger UI active");
    CHECK(s.gta.starts == 0 && s.body.GetPhase() == body::Phase::Idle, "no pose for passengers");
    s.game.isDriver = true;                       // moved to the driver seat
    s.Run(400);
    CHECK(s.c.GetState() == State::Hidden, "driver seat closes the UI");
    // On foot with the phone out, then entering a vehicle as passenger.
    Sim p;
    p.Tap(); p.Run(1500);
    p.game.inVehicle = true;
    p.Run(16);
    CHECK(p.gta.quits == 1, "entering a vehicle puts the phone away");
    CHECK(p.c.OwnsInput(), "UI stays usable as passenger");
    p.Run(3000);
    CHECK(p.Clean(), "no phone left in hand in the vehicle");
    CHECK(s.NoViolations() && p.NoViolations(), "no violations for vehicle policy");
}

void TestPauseAndFocus() {
    Sim s;
    s.Tap(); s.Run(1500);
    s.c.Suspend();                                // ESC pause menu / Alt+Tab
    s.Run(16);
    CHECK(s.gta.quits == 1, "suspension puts the phone away");
    s.Run(3000);
    CHECK(s.Clean(), "nothing attached while suspended");
    CHECK(s.Tap() == KeyResult::Opened, "L after pause reopens");
    s.Run(1500);
    CHECK(s.gta.starts == 2 && s.gta.prop, "phone back out after pause");
    s.c.Suspend(); s.Run(16);
    s.c.Resume(s.t); s.Run(3000);                 // focus regained mid phone_out
    CHECK(s.gta.starts == 3 && s.gta.prop && s.gta.maxLive <= 1, "focus resume restarts once");
    CHECK(s.NoViolations(), "no violations on pause/focus");
}

void TestForeignAndFlush() {
    Sim s;
    s.gta.slot = 0x9000;                          // server cellphone action / script task
    s.Tap(); s.Run(1500);
    CHECK(s.c.OwnsInput(), "UI works next to a foreign task");
    CHECK(s.gta.starts == 0 && s.body.GetPhase() == body::Phase::Foreign, "foreign slot: UI only");
    s.Tap(); s.Run(500);
    CHECK(s.gta.quits == 0 && s.gta.aborts == 0 && s.gta.slot == 0x9000, "foreign task never touched");
    s.gta.slot = 0;
    // Server ClearAnimations flushes our task while the phone is open.
    s.Tap(); s.Run(1500);
    CHECK(s.gta.starts == 1, "own task started once the slot is free");
    s.gta.Clear();
    s.Run(1000);
    CHECK(s.gta.starts == 1 && s.body.Lost() == 1, "flushed task is not restarted this session");
    s.Tap(); s.Run(500); s.Tap(); s.Run(1500);
    CHECK(s.gta.starts == 2, "next open session starts again");
    CHECK(s.NoViolations(), "no violations with foreign/flushed tasks");
}

void TestBrowsePose() {
    Sim s;
    CHECK(s.body.GetHoldPose().enabled, "browsing pose enabled by default");
    CHECK(s.body.GetHoldPose().atSec < 0.85f, "browse freeze below natural model time");
    s.Tap(); s.Run(500);
    CHECK(!s.gta.prop, "no model before freeze point");
    s.Run(300); // total ~800 ms: past the 650 ms freeze
    CHECK(s.gta.prop && s.gta.earlyProp, "model attached early at browsing height");
    CHECK(s.gta.holding, "phone_in frozen");
    CHECK(s.gta.sub == MockGta::Sub::In, "still in phone_in (no phone_talk)");
    s.Tap(); s.Run(4000);
    CHECK(s.Clean(), "browsing pose closes cleanly");
    CHECK(s.NoViolations(), "no violations with browsing pose");
}

void TestLegacyHoldPose() {
    Sim s;
    body::HoldPose h; h.enabled = true; h.atSec = 1.0f;
    s.body.SetHoldPose(h);
    s.Tap(); s.Run(3000);
    CHECK(s.gta.holdOn > 0 && s.gta.holding, "hold requested while active");
    CHECK(s.gta.sub == MockGta::Sub::In && s.gta.prop, "held in phone_in with the model in hand");
    CHECK(!s.gta.earlyProp, "model at natural time (1.0 >= 0.85)");
    s.Tap(); s.Frame();
    CHECK(s.gta.holdOff >= 1 && !s.gta.holding, "hold released before phone_out");
    s.Run(4000);
    CHECK(s.Clean(), "hold pose ends clean");
    CHECK(s.NoViolations(), "no violations with legacy hold pose");
}

void TestEarMode() {
    Sim s;
    s.body.SetHoldPose({false, 0.65f}); // disabled: phone_talk (ear) plays
    s.Tap(); s.Run(2000);
    CHECK(s.gta.prop, "model in hand via natural 0.85 timing");
    CHECK(!s.gta.earlyProp, "no early model when hold disabled");
    CHECK(!s.gta.holding, "no hold active");
    CHECK(s.gta.sub == MockGta::Sub::Talk, "phone_talk (ear) playing");
    s.Tap(); s.Run(4000);
    CHECK(s.Clean(), "ear mode closes cleanly");
    CHECK(s.NoViolations(), "no violations with ear mode");
}

void TestHiddenIsFree() {
    Sim s;
    s.Run(16000);
    CHECK(s.gta.calls == 0, "hidden phone makes zero game calls");
    s.Tap(); s.Run(1500); s.Tap(); s.Run(3000);
    const int after = s.gta.calls;
    s.Run(16000);
    CHECK(s.gta.calls == after, "zero game calls once closed and released");
}

void MeasureOverhead() {
    MockGta gta;
    body::Coordinator body(gta);
    body::Request want; want.want = true;
    body.Update(want);
    constexpr int kN = 2000000;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kN; ++i) body.Update(want);
    const auto t1 = std::chrono::steady_clock::now();
    body::Request idle;
    body::Coordinator idleBody(gta);
    const auto t2 = std::chrono::steady_clock::now();
    for (int i = 0; i < kN; ++i) idleBody.Update(idle);
    const auto t3 = std::chrono::steady_clock::now();
    const double active = std::chrono::duration<double, std::nano>(t1 - t0).count() / kN;
    const double hidden = std::chrono::duration<double, std::nano>(t3 - t2).count() / kN;
    std::printf("  coordinator cost (mock backend): active %.1f ns/frame, hidden %.1f ns/frame\n", active, hidden);
    CHECK(active < 5000.0, "coordinator update is far below a frame budget");
}

} // namespace

int main() {
    TestOpenCloseOnce();
    TestTwentyCycles();
    TestRapidToggle();
    TestInterrupted();
    TestDeath();
    TestDisconnectAndRespawn();
    TestDriverAndPassenger();
    TestPauseAndFocus();
    TestForeignAndFlush();
    TestBrowsePose();
    TestLegacyHoldPose();
    TestEarMode();
    TestHiddenIsFree();
    MeasureOverhead();
    std::printf("\nPhone M3/M5.9 tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
