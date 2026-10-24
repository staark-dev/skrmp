// API-003.3 — GPS world checkpoint state (gps_checkpoint_model.h): exact
// waypoint position, ground height gating, draw distance, arrival policies,
// cancel cleanup, server create/modify/remove, de-duplication, advisory
// arrival, and a source scan of the per-frame placement path.
// Exit 0 = pass, 1 = fail.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "gps/gps_checkpoint_model.h"

using namespace skr::gps;
namespace C = skr::gps::checkpoint;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { if (cond) ++g_pass; else { ++g_fail; std::printf("  FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

static int Collect(C::Controller& c, Vec3 player, uint32_t now, C::Placement* out) {
    return c.Collect(player, now, out, C::kSlots);
}

static void TestLocal() {
    std::puts("== Local waypoint marker ==");
    C::Controller c;
    C::Placement p[C::kSlots];
    const Vec3 wp{1807.0f, -645.0f, 0.0f};          // off-road waypoint (map z = 0)
    const Vec3 near{1790.0f, -640.0f, 25.0f};

    c.SyncLocal(true, wp, 1000);
    CHECK(c.Slot(0).active && c.Slot(0).pos.x == wp.x && c.Slot(0).pos.y == wp.y,
          "marker at the exact waypoint (not the road-snapped route end)");
    CHECK(Collect(c, near, 1000, p) == 0, "no ground height yet: not placed (never at z = 0)");
    CHECK(c.NeedsGround(0, near), "ground lookup requested near the player");
    CHECK(!c.NeedsGround(0, Vec3{-2000, 500, 10}), "no ground lookup far away (collision not streamed)");
    c.SetGround(0, 23.5f);
    CHECK(Collect(c, near, 1000, p) == 1 && p[0].slot == 0 && p[0].pos.z == 23.5f && p[0].pos.x == wp.x,
          "placed at waypoint x/y with ground z");
    CHECK(Collect(c, Vec3{1807.0f - 400.0f, -645.0f, 20.0f}, 1000, p) == 0, "beyond draw distance (250): not placed");

    // Re-sync with the same waypoint: no reset of the ground height (no re-query churn).
    c.SyncLocal(true, wp, 1100);
    CHECK(c.Slot(0).groundKnown, "same waypoint: state kept");
    // Moved waypoint: new position, ground re-queried.
    c.SyncLocal(true, Vec3{1900.0f, -600.0f, 0.0f}, 1200);
    CHECK(c.Slot(0).pos.x == 1900.0f && !c.Slot(0).groundKnown, "moved waypoint: marker follows, ground re-queried");

    // Cancel: removed whatever the policy.
    c.SetLocalPolicy(C::ArrivalPolicy::KeepUntilCleared, 0);
    c.SyncLocal(true, wp, 1300);
    c.SetGround(0, 23.5f);
    c.OnLocalCancelled();
    c.SyncLocal(false, Vec3{}, 1300);
    CHECK(!c.Slot(0).active && Collect(c, near, 1300, p) == 0, "cancel: removed (even with KeepUntilCleared)");

    // Waypoint cleared without an arrival event (player removed it on the map).
    c.SyncLocal(true, wp, 1400);
    c.SyncLocal(false, Vec3{}, 1500);
    CHECK(!c.Slot(0).active, "waypoint gone without arrival: removed");
}

static void TestArrivalPolicies() {
    std::puts("== Arrival policies ==");
    C::Placement p[C::kSlots];
    const Vec3 wp{100.0f, 100.0f, 0.0f}, at{101.0f, 100.0f, 10.0f};
    for (int policy = 0; policy < 3; ++policy) {
        C::Controller c;
        c.SetLocalPolicy(static_cast<C::ArrivalPolicy>(policy), 2000);
        c.SyncLocal(true, wp, 1000);
        c.SetGround(0, 10.0f);
        c.OnLocalArrived(5000);              // engine emits Arrived, then clears the waypoint
        c.SyncLocal(false, Vec3{}, 5000);
        const int now = Collect(c, at, 5000, p);
        const int later = Collect(c, at, 7100, p);
        if (policy == 0) CHECK(now == 0 && !c.Slot(0).active, "Remove: gone on arrival");
        if (policy == 1) CHECK(now == 1 && later == 0, "KeepFor 2 s: kept, then removed");
        if (policy == 2) CHECK(now == 1 && later == 1, "KeepUntilCleared: kept after arrival");
        if (policy == 2) {
            c.SyncLocal(true, Vec3{300.0f, 300.0f, 0.0f}, 8000);
            CHECK(c.Slot(0).active && !c.Slot(0).arrived && c.Slot(0).pos.x == 300.0f, "new waypoint replaces the kept marker");
            c.ClearLocal();
            CHECK(!c.Slot(0).active, "explicit clear");
        }
    }
}

static void TestServer() {
    std::puts("== Server destinations (API-003.3 draft) ==");
    C::Controller c;
    C::Placement p[C::kSlots];
    C::Style s; s.r = 230; s.g = 60; s.b = 60; s.size = 3.0f;
    const Vec3 player{0, 0, 10};
    CHECK(!c.SetServer(0, Vec3{1, 1, 1}, true, s, C::ArrivalPolicy::Remove, 0, 0), "id 0 rejected (local slot)");
    CHECK(!c.SetServer(C::kMaxServer + 1, Vec3{1, 1, 1}, true, s, C::ArrivalPolicy::Remove, 0, 0), "id above max rejected");
    CHECK(!c.SetServer(1, Vec3{NAN, 1, 1}, true, s, C::ArrivalPolicy::Remove, 0, 0), "NaN rejected");
    CHECK(!c.SetServer(1, Vec3{4000, 1, 1}, true, s, C::ArrivalPolicy::Remove, 0, 0), "outside the map rejected");
    C::Style bad = s; bad.size = 0;
    CHECK(!c.SetServer(1, Vec3{1, 1, 1}, true, bad, C::ArrivalPolicy::Remove, 0, 0), "zero size rejected");

    CHECK(c.SetServer(1, Vec3{20, 0, 12}, true, s, C::ArrivalPolicy::KeepUntilCleared, 0, 5.0f), "create");
    CHECK(Collect(c, player, 100, p) == 1 && p[0].slot == 1 && p[0].pos.z == 12.0f && p[0].style.r == 230,
          "server marker with server z and style (no ground lookup)");
    s.r = 10;
    CHECK(c.SetServer(1, Vec3{20, 0, 12}, true, s, C::ArrivalPolicy::KeepUntilCleared, 0, 5.0f), "modify style");
    CHECK(Collect(c, player, 100, p) == 1 && p[0].style.r == 10, "modified in place (still one marker)");
    CHECK(c.SetServer(2, Vec3{-20, 0, 0}, false, s, C::ArrivalPolicy::Remove, 0, 0), "second id, no z");
    CHECK(Collect(c, player, 100, p) == 1, "no-z server marker waits for its ground height");

    // De-duplication: local waypoint on top of a server destination → one marker.
    c.SyncLocal(true, Vec3{21, 0, 0}, 100);
    c.SetGround(0, 12.0f);
    CHECK(Collect(c, player, 100, p) == 1, "local waypoint at a server destination: one marker, not two");

    // Advisory arrival (once), policy applied.
    int id = -1;
    c.UpdateServerArrival(Vec3{18, 0, 12}, 200);
    c.UpdateServerArrival(Vec3{18, 0, 12}, 300);
    CHECK(c.TakeArrival(id) && id == 1 && !c.TakeArrival(id), "entry reported once (advisory)");
    c.UpdateServerArrival(Vec3{20, 0, 60}, 300);
    CHECK(!c.TakeArrival(id), "different level (z): no entry");
    CHECK(c.RemoveServer(1) && !c.RemoveServer(1), "remove, then remove again fails (no stale handle)");
    c.ClearServer();
    CHECK(!c.Slot(2).active, "clear all server markers");
    c.Reset();
    CHECK(!c.Slot(0).active && !c.Slot(1).active, "reset: nothing left");
}

static void TestVisibility() {
    std::puts("== Style / visibility ==");
    C::Controller c;
    C::Placement p[C::kSlots];
    C::Style hidden; hidden.visible = false;
    c.SetLocalStyle(hidden);
    c.SyncLocal(true, Vec3{5, 5, 0}, 1);
    c.SetGround(0, 3);
    CHECK(Collect(c, Vec3{0, 0, 3}, 1, p) == 0, "visible = false: not placed");
    C::Style wide; wide.drawDistance = 900; wide.size = 6;
    c.SetLocalStyle(wide);
    CHECK(Collect(c, Vec3{800, 5, 3}, 1, p) == 1 && p[0].style.size == 6, "configured draw distance and size");
}

static std::string ReadSource(const char* rel) {
    std::string path = __FILE__;
    path = path.substr(0, path.find_last_of("\\/")) + "/../../" + rel;
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

static void TestSource() {
    std::puts("== Placement path: no I/O, no hooks ==");
    const std::string src = ReadSource("client/gps/gps_checkpoint.cpp");
    CHECK(!src.empty(), "source found");
    for (const char* bad : {"fopen", "CreateFile", "ofstream", "Log(", "VirtualProtect", "new ", "malloc", "std::vector"})
        CHECK(src.find(bad) == std::string::npos, bad);
    CHECK(src.find("kMarkerCylinder       = 1") != std::string::npos, "cylinder type (never evicts other markers)");
}

int main() {
    TestLocal();
    TestArrivalPolicies();
    TestServer();
    TestVisibility();
    TestSource();
    std::printf("\ngps checkpoint: %d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
