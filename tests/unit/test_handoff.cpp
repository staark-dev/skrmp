// 120 s handoff fix: Present hook chain (fake + real D3D9 vtables) and the
// launcher watchdog policy. Exit 0 = pass, 1 = fail.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#include <thread>

#include "render-d3d9/present_chain.h"
#include "launch_watchdog.h"
#include "ipc/launch_params.h"
#include "ipc/launch_milestones.h"

namespace pc = skr::render::present_chain;
using skr::launcher::WatchdogAction;
using skr::launcher::WatchdogInput;
using skr::launcher::DecideWatchdog;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { if (cond) ++g_pass; else { ++g_fail; std::printf("  FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

// ── Fake Present targets ────────────────────────────────────────────
static void* g_vtA[20];   // "real device" vtable
static void* g_vtB[20];   // "SA-MP proxy" vtable
static int g_real = 0, g_samp = 0, g_handler = 0, g_maxDepthSeen = 0;
static bool g_cefOwner = false;

static HRESULT WINAPI RealPresent(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*) {
    ++g_real; return S_OK;
}
// SA-MP proxy forwarding through the real device's (hooked) vtable.
static HRESULT WINAPI SampViaVtable(IDirect3DDevice9* d, const RECT* s, const RECT* t, HWND w, const RGNDATA* r) {
    ++g_samp;
    return reinterpret_cast<pc::PresentFn>(g_vtA[pc::kPresentIndex])(d, s, t, w, r);
}
// SA-MP calling a Present pointer it captured before we hooked.
static HRESULT WINAPI SampDirect(IDirect3DDevice9* d, const RECT* s, const RECT* t, HWND w, const RGNDATA* r) {
    ++g_samp;
    return RealPresent(d, s, t, w, r);
}
static HRESULT Handler(pc::PresentFn next, IDirect3DDevice9* d, const RECT* s, const RECT* t, HWND w, const RGNDATA* r) {
    if (g_cefOwner) return next(d, s, t, w, r);
    ++g_handler;
    if (!pc::InFrame()) g_maxDepthSeen = -1;
    return next(d, s, t, w, r);
}
static HRESULT CallEntry(void** vt) {
    return reinterpret_cast<pc::PresentFn>(vt[pc::kPresentIndex])(nullptr, nullptr, nullptr, nullptr, nullptr);
}
static void ResetCounters() { g_real = g_samp = g_handler = 0; }
static void ResetVtables() {
    for (auto& e : g_vtA) e = nullptr;
    for (auto& e : g_vtB) e = nullptr;
    g_vtA[pc::kPresentIndex] = reinterpret_cast<void*>(&RealPresent);
}

static void TestFakeChain() {
    std::puts("== Present chain (fake vtables) ==");
    pc::SetFrameHandler(&Handler);

    // Scenario 1: proxy forwards through the hooked real vtable.
    ResetVtables();
    CHECK(pc::Install(g_vtA, pc::Primary) == pc::InstallResult::Installed, "primary installed");
    ResetCounters(); CallEntry(g_vtA);
    CHECK(g_handler == 1 && g_real == 1, "primary: one handler pass, one real Present");
    g_vtB[pc::kPresentIndex] = reinterpret_cast<void*>(&SampViaVtable);   // SA-MP proxy appears
    CHECK(pc::Install(g_vtB, pc::Rechained) == pc::InstallResult::Installed, "re-chained onto proxy vtable");
    ResetCounters();
    for (int i = 0; i < 1000; ++i) CallEntry(g_vtB);
    CHECK(g_handler == 1000 && g_samp == 1000 && g_real == 1000, "proxy->hooked real: exactly once per frame, no recursion");
    CHECK(g_maxDepthSeen == 0, "handler always runs inside the frame guard");
    CHECK(pc::Install(g_vtB, pc::Rechained) == pc::InstallResult::AlreadyOurs, "no duplicate on proxy vtable");
    CHECK(pc::Install(g_vtA, pc::Rechained) == pc::InstallResult::AlreadyOurs, "no duplicate on real vtable");
    void* other[20] = {}; other[pc::kPresentIndex] = reinterpret_cast<void*>(&RealPresent);
    CHECK(pc::Install(other, pc::Rechained) == pc::InstallResult::SlotInUse, "slot cannot be installed twice");
    pc::Uninstall();
    CHECK(g_vtA[pc::kPresentIndex] == reinterpret_cast<void*>(&RealPresent) &&
          g_vtB[pc::kPresentIndex] == reinterpret_cast<void*>(&SampViaVtable), "uninstall restores both entries");

    // Scenario 2: SA-MP calls a captured real Present directly (primary hook bypassed).
    ResetVtables();
    pc::Install(g_vtA, pc::Primary);
    g_vtB[pc::kPresentIndex] = reinterpret_cast<void*>(&SampDirect);
    ResetCounters(); CallEntry(g_vtB);
    CHECK(g_handler == 0 && g_real == 1, "before re-chain: SKR frame work lost (the 120 s bug)");
    CHECK(pc::Install(g_vtB, pc::Rechained) == pc::InstallResult::Installed, "re-chain installs");
    ResetCounters(); CallEntry(g_vtB);
    CHECK(g_handler == 1 && g_samp == 1 && g_real == 1, "after re-chain: frame work restored");
    pc::Uninstall();

    // Scenario 3: SA-MP overwrote the entry in the same vtable.
    ResetVtables();
    pc::Install(g_vtA, pc::Primary);
    g_vtA[pc::kPresentIndex] = reinterpret_cast<void*>(&SampDirect);
    CHECK(pc::Install(g_vtA, pc::Rechained) == pc::InstallResult::Installed, "re-chain into same vtable");
    ResetCounters(); CallEntry(g_vtA);
    CHECK(g_handler == 1 && g_real == 1, "same-vtable overwrite: once per frame");
    pc::Uninstall();
    CHECK(g_vtA[pc::kPresentIndex] == reinterpret_cast<void*>(&SampDirect), "uninstall leaves SA-MP's entry, not stale ours");

    // Someone hooks over us: uninstall must not clobber them.
    ResetVtables();
    pc::Install(g_vtA, pc::Primary);
    g_vtA[pc::kPresentIndex] = reinterpret_cast<void*>(&SampDirect);
    pc::Uninstall();
    CHECK(g_vtA[pc::kPresentIndex] == reinterpret_cast<void*>(&SampDirect), "foreign hook over ours is preserved");

    // CEF owner: handler passes straight through, Present still happens.
    ResetVtables();
    pc::Install(g_vtA, pc::Primary);
    g_cefOwner = true; ResetCounters(); CallEntry(g_vtA); g_cefOwner = false;
    CHECK(g_handler == 0 && g_real == 1, "CEF owner: pass-through, no SKR frame work");

    // No handler: plain pass-through.
    pc::SetFrameHandler(nullptr); ResetCounters(); CallEntry(g_vtA);
    CHECK(g_real == 1, "no handler: pass-through");
    pc::SetFrameHandler(&Handler);
    pc::Uninstall();

    CHECK(pc::Install(nullptr, pc::Primary) == pc::InstallResult::Failed, "null vtable rejected");
    void* empty[20] = {};
    CHECK(pc::Install(empty, pc::Primary) == pc::InstallResult::Failed, "null entry rejected");
}

// ── Real D3D9 device ────────────────────────────────────────────────
static pc::PresentFn g_sampCaptured = nullptr;
static int g_sampReal = 0;
static HRESULT WINAPI SampRealDevice(IDirect3DDevice9* d, const RECT* s, const RECT* t, HWND w, const RGNDATA* r) {
    ++g_sampReal;
    return g_sampCaptured(d, s, t, w, r);
}

static void TestRealDevice() {
    std::puts("== Present chain (real D3D9 device, repeated Reset) ==");
    WNDCLASSA wc{}; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(nullptr); wc.lpszClassName = "SKRHandoffTest";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA(wc.lpszClassName, "", WS_OVERLAPPEDWINDOW, 0, 0, 320, 240, nullptr, nullptr, wc.hInstance, nullptr);
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS pp{}; pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = hwnd;
    pp.BackBufferWidth = 320; pp.BackBufferHeight = 240;
    IDirect3DDevice9* dev = nullptr;
    if (!d3d || FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                         D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev)) || !dev) {
        std::puts("  SKIPPED: no HAL device");
        return;
    }
    void** vt = *reinterpret_cast<void***>(dev);
    pc::SetFrameHandler(&Handler);
    CHECK(pc::Install(vt, pc::Primary) == pc::InstallResult::Installed, "primary on real vtable");
    ResetCounters();
    for (int i = 0; i < 20; ++i) { dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1, 0); CHECK(SUCCEEDED(dev->Present(nullptr, nullptr, nullptr, nullptr)), "Present ok"); }
    CHECK(g_handler == 20, "20 real frames through the primary hook");

    // SA-MP-style takeover: entry replaced by a hook that calls a captured original.
    g_sampCaptured = pc::Original(pc::Primary);
    DWORD old = 0;
    VirtualProtect(&vt[pc::kPresentIndex], sizeof(void*), PAGE_READWRITE, &old);
    vt[pc::kPresentIndex] = reinterpret_cast<void*>(&SampRealDevice);
    VirtualProtect(&vt[pc::kPresentIndex], sizeof(void*), old, &old);
    ResetCounters(); g_sampReal = 0;
    dev->Present(nullptr, nullptr, nullptr, nullptr);
    CHECK(g_handler == 0 && g_sampReal == 1, "takeover bypasses SKR before re-chain");
    CHECK(pc::Install(vt, pc::Rechained) == pc::InstallResult::Installed, "re-chain on real vtable");

    ResetCounters(); g_sampReal = 0;
    int frames = 0, resets = 0;
    for (int round = 0; round < 5; ++round) {
        for (int i = 0; i < 10; ++i) {
            dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1, 0);
            if (SUCCEEDED(dev->Present(nullptr, nullptr, nullptr, nullptr))) ++frames;
        }
        D3DPRESENT_PARAMETERS rp = pp;
        rp.BackBufferWidth = 320 + 16 * (round % 2); rp.BackBufferHeight = 240;
        if (SUCCEEDED(dev->Reset(&rp))) ++resets;
        CHECK(pc::IsOurHook(vt[pc::kPresentIndex]), "hook survives Reset");
    }
    CHECK(frames == 50 && resets == 5, "50 frames across 5 Resets");
    CHECK(g_handler == 50 && g_sampReal == 50, "re-chained: SKR work once per frame across Resets");

    pc::Uninstall();
    CHECK(vt[pc::kPresentIndex] == reinterpret_cast<void*>(&SampRealDevice), "uninstall restores SA-MP entry");
    VirtualProtect(&vt[pc::kPresentIndex], sizeof(void*), PAGE_READWRITE, &old);
    vt[pc::kPresentIndex] = reinterpret_cast<void*>(g_sampCaptured);
    VirtualProtect(&vt[pc::kPresentIndex], sizeof(void*), old, &old);
    dev->Release(); d3d->Release(); DestroyWindow(hwnd);
}

// ── Watchdog policy ─────────────────────────────────────────────────
static WatchdogAction At(bool armed, bool visual, bool gameplay, uint64_t t) {
    WatchdogInput in; in.armed = armed; in.visualReady = visual; in.gameplayConfirmed = gameplay;
    in.now = 1000 + t; in.deadline = 1000 + skr::launcher::kHandoffTimeoutMs;
    return DecideWatchdog(in);
}

static void TestWatchdog() {
    std::puts("== Launcher watchdog ==");
    CHECK(At(true, false, false, 60000) == WatchdogAction::None, "armed, before deadline: wait");
    CHECK(At(true, false, false, 120000) == WatchdogAction::Terminate, "failed launch still terminated at 120 s");
    CHECK(At(true, true, false, 5000) == WatchdogAction::DisarmVisualReady, "visual-ready disarms");
    CHECK(At(true, false, true, 20000) == WatchdogAction::DisarmGameplay, "gameplay confirmation disarms");
    CHECK(At(true, false, true, 500000) == WatchdogAction::DisarmGameplay, "confirmed late but before check: disarm, not kill");
    CHECK(At(false, false, false, 180000) == WatchdogAction::None, "disarmed never terminates (180 s)");
    CHECK(At(false, false, false, 3600000) == WatchdogAction::None, "disarmed never terminates (1 h)");

    // Replay of the failing runs: visual-ready never arrives, gameplay confirmed ~20 s in.
    bool armed = true, killed = false;
    for (uint64_t t = 0; t <= 200000; t += 16) {
        const bool gameplay = t >= 20000;
        switch (At(armed, false, gameplay, t)) {
            case WatchdogAction::DisarmGameplay:
            case WatchdogAction::DisarmVisualReady: armed = false; break;
            case WatchdogAction::Terminate: killed = true; armed = false; break;
            default: break;
        }
    }
    CHECK(!killed, "confirmed session without visual-ready survives past 180 s");

    // Stuck launch: neither signal ever arrives.
    armed = true; killed = false; uint64_t killedAt = 0;
    for (uint64_t t = 0; t <= 200000 && !killed; t += 16)
        if (At(armed, false, false, t) == WatchdogAction::Terminate) { killed = true; killedAt = t; }
    CHECK(killed && killedAt >= 120000 && killedAt < 120016, "stuck launch terminated at 120 s");
}

static void TestIpc() {
    std::puts("== Launch params v6 ==");
    using namespace skr::ipc;
    CHECK(kLaunchParamsVersion == 6, "version bumped for sessionId + milestone handles");
    LaunchParams p{};
    p.magic = kLaunchParamsMagic; p.version = kLaunchParamsVersion; p.serverPort = 7777;
    strcpy_s(p.serverAddress, "127.0.0.1"); strcpy_s(p.playerName, "Staark");
    p.gameplayConfirmedHandle = 0x1234;
    p.clientInitializedHandle = 0x10; p.sampLoadedHandle = 0x20; p.sampReadyHandle = 0x30;
    p.sessionId = 0x5EC0DE1234ABCDEFull;
    bool wrote = false;
    LaunchParams got{};
    const DWORD pid = GetCurrentProcessId();
    std::thread writer([&] { wrote = WriteLaunchParams(pid, p, 2000); });
    const bool read = ReadLaunchParams(got, 2000);
    writer.join();
    CHECK(wrote && read, "v6 round trip; writer verified the echoed session id");
    CHECK(got.gameplayConfirmedHandle == 0x1234 && got.clientInitializedHandle == 0x10 &&
          got.sampLoadedHandle == 0x20 && got.sampReadyHandle == 0x30, "milestone handles cross the pipe");
    CHECK(got.sessionId == p.sessionId, "client knows the launch session id");
    p.version = 5;
    CHECK(!WriteLaunchParams(pid, p, 150), "v5 peer rejected (no silent layout mismatch)");
}

static void TestMilestones() {
    std::puts("== Readiness milestones ==");
    using skr::ipc::Milestone;
    using R = skr::ipc::MilestoneTracker::Result;
    skr::ipc::MilestoneTracker t;
    const uint64_t s = 0xABCDEF01ull;
    t.Reset(s, 1000);
    CHECK(t.Observe(Milestone::ClientInitialized, s, 1100) == R::Recorded, "client initialized");
    CHECK(t.Observe(Milestone::SampLoaded, s, 1500) == R::Recorded, "samp loaded");
    CHECK(t.Observe(Milestone::SampReady, s, 9000) == R::Recorded, "samp ready");
    CHECK(!t.HealthyGameplay() && !t.StartupConfirmed(), "SAMP_READY alone never certifies gameplay");
    CHECK(t.Observe(Milestone::SampReady, s, 9100) == R::Duplicate, "duplicate ignored");
    CHECK(t.ElapsedMs(Milestone::SampReady) == 8000, "first timestamp kept");
    CHECK(t.Observe(Milestone::GameplayConfirmed, s, 20000) == R::Recorded, "gameplay confirmed");
    CHECK(t.HealthyGameplay() && t.StartupConfirmed(), "gameplay confirmed = healthy");
    CHECK(t.Observe(Milestone::SessionReady, s, 20010) == R::Recorded, "session ready after confirmation");
    CHECK(t.Observe(Milestone::VisualReady, 0x1111, 21000) == R::StaleSession, "other session's signal ignored");
    CHECK(!t.Has(Milestone::VisualReady), "stale signal not recorded");
    CHECK(t.Observe(Milestone::VisualReady, 0, 21000) == R::StaleSession, "zero session ignored");

    // Missing prerequisites are recorded but flagged.
    t.Reset(s, 0);
    CHECK(t.Observe(Milestone::SampReady, s, 10) == R::RecordedOutOfOrder, "SAMP_READY before SAMP_LOADED flagged");
    CHECK(t.Observe(Milestone::SessionReady, s, 20) == R::RecordedOutOfOrder, "SESSION_READY without visual/gameplay flagged");
    t.Reset(s, 0);
    CHECK(!t.Has(Milestone::SampReady) && !t.Has(Milestone::SessionReady), "reset clears the session");
    CHECK(t.Observe(Milestone::VisualReady, s, 5) == R::RecordedOutOfOrder && t.StartupConfirmed(),
          "visual-ready confirms startup even if earlier events were missed");

    // Delayed gameplay against the watchdog, driven by the tracker.
    auto run = [&](uint64_t gameplayAt) {
        skr::ipc::MilestoneTracker m; m.Reset(s, 0);
        bool armed = true, killed = false;
        for (uint64_t now = 0; now <= 200000; now += 16) {
            if (now >= gameplayAt) m.Observe(Milestone::GameplayConfirmed, s, now);
            if (now >= 3000) m.Observe(Milestone::SampReady, s, now);
            WatchdogInput in; in.armed = armed; in.visualReady = m.Has(Milestone::VisualReady);
            in.gameplayConfirmed = m.HealthyGameplay(); in.now = 1000 + now; in.deadline = 1000 + skr::launcher::kHandoffTimeoutMs;
            switch (DecideWatchdog(in)) {
                case WatchdogAction::Terminate: killed = true; armed = false; break;
                case WatchdogAction::DisarmGameplay: case WatchdogAction::DisarmVisualReady: armed = false; break;
                default: break;
            }
            if (killed) break;
        }
        return killed;
    };
    CHECK(!run(119000), "gameplay at 119 s: kept alive past 200 s");
    CHECK(run(121000), "gameplay only at 121 s: startup failed, terminated at 120 s");
    CHECK(run(500000), "SAMP_READY without gameplay: terminated at 120 s");
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    TestFakeChain();
    TestRealDevice();
    TestWatchdog();
    TestIpc();
    TestMilestones();
    std::printf("\nhandoff: %d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
