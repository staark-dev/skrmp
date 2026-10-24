// SKR-X VEHICLES M2.0 unit tests — protocol codec + vehicle manager.
// Build: SKRXVehiclesTests target in server/skr-gate-omp/CMakeLists.txt.
// No open.mp runtime, no GTA, no network.

#include "protocol/skrx_vehicle.h"
#include "skrx_vehicles_server.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

static int g_checks = 0;
static int g_fails = 0;

#define CHECK(expr, label) do { \
    ++g_checks; \
    if (!(expr)) { \
        ++g_fails; \
        std::printf("FAIL [%s] line %d: %s\n", label, __LINE__, #expr); \
    } \
} while (0)

namespace proto = skr::protocol::skrx::vehicles;
using skr::gate_omp::vehicles::VehicleManager;

// ═══════════════════════════════════════════════════════════════════
// A. Protocol codec tests
// ═══════════════════════════════════════════════════════════════════

static void TestVehicleAssignCodec() {
    proto::VehicleAssignData a;
    a.vehiclePoolId = 42;
    a.customModelId = 612;
    a.generation = 0xDEADBEEF;
    a.flags = 3;

    uint8_t buf[32];
    size_t n = proto::EncodeVehicleAssign(a, buf, sizeof(buf));
    CHECK(n == proto::kVehicleAssignSize, "assign-size");

    proto::VehicleAssignData b;
    CHECK(proto::DecodeVehicleAssign(buf, n, b), "assign-decode");
    CHECK(b.vehiclePoolId == 42, "assign-poolid");
    CHECK(b.customModelId == 612, "assign-model");
    CHECK(b.generation == 0xDEADBEEF, "assign-gen");
    CHECK(b.flags == 3, "assign-flags");

    // Short buffer
    CHECK(proto::EncodeVehicleAssign(a, buf, 4) == 0, "assign-short-encode");
    CHECK(!proto::DecodeVehicleAssign(buf, 5, b), "assign-short-decode");
}

static void TestVehicleRevokeCodec() {
    uint8_t buf[8];
    size_t n = proto::EncodeVehicleRevoke(1234, buf, sizeof(buf));
    CHECK(n == proto::kVehicleRevokeSize, "revoke-size");

    uint16_t id = 0;
    CHECK(proto::DecodeVehicleRevoke(buf, n, id), "revoke-decode");
    CHECK(id == 1234, "revoke-id");

    CHECK(!proto::DecodeVehicleRevoke(buf, 1, id), "revoke-short");
}

static void TestVehicleReadyCodec() {
    proto::VehicleReadyData r;
    r.vehiclePoolId = 100;
    r.customModelId = 613;
    r.generation = 7;
    r.status = 1;

    uint8_t buf[32];
    size_t n = proto::EncodeVehicleReady(r, buf, sizeof(buf));
    CHECK(n == proto::kVehicleReadySize, "ready-size");

    proto::VehicleReadyData s;
    CHECK(proto::DecodeVehicleReady(buf, n, s), "ready-decode");
    CHECK(s.vehiclePoolId == 100, "ready-poolid");
    CHECK(s.customModelId == 613, "ready-model");
    CHECK(s.generation == 7, "ready-gen");
    CHECK(s.status == 1, "ready-status");
}

static void TestModelRegistryCodec() {
    proto::ModelRegistryEntry entries[3];

    entries[0].customModelId = 612;
    entries[0].proxyModelId = 400;
    entries[0].donorModelId = 400;
    entries[0].nameLen = 8;
    memcpy(entries[0].internalName, "skr_xc60", 8);

    entries[1].customModelId = 613;
    entries[1].proxyModelId = 475;
    entries[1].donorModelId = 475;
    entries[1].nameLen = 9;
    memcpy(entries[1].internalName, "skr_m4gts", 9);

    entries[2].customModelId = 614;
    entries[2].proxyModelId = 560;
    entries[2].donorModelId = 560;
    entries[2].nameLen = 7;
    memcpy(entries[2].internalName, "skr_rs6", 7);

    uint8_t buf[256];
    size_t n = proto::EncodeModelRegistry(entries, 3, buf, sizeof(buf));
    CHECK(n > 0, "registry-encode");
    CHECK(buf[0] == 3, "registry-count");

    proto::ModelRegistryEntry decoded[32];
    uint8_t count = 0;
    CHECK(proto::DecodeModelRegistry(buf, n, decoded, count), "registry-decode");
    CHECK(count == 3, "registry-decode-count");
    CHECK(decoded[0].customModelId == 612, "registry-m0-id");
    CHECK(decoded[0].proxyModelId == 400, "registry-m0-proxy");
    CHECK(decoded[0].donorModelId == 400, "registry-m0-donor");
    CHECK(decoded[0].nameLen == 8, "registry-m0-namelen");
    CHECK(memcmp(decoded[0].internalName, "skr_xc60", 8) == 0, "registry-m0-name");
    CHECK(decoded[1].customModelId == 613, "registry-m1-id");
    CHECK(decoded[2].customModelId == 614, "registry-m2-id");

    // Empty registry
    size_t n0 = proto::EncodeModelRegistry(entries, 0, buf, sizeof(buf));
    CHECK(n0 == 1, "registry-empty-size");
    CHECK(buf[0] == 0, "registry-empty-count");
    CHECK(proto::DecodeModelRegistry(buf, 1, decoded, count), "registry-empty-decode");
    CHECK(count == 0, "registry-empty-count2");
}

static void TestBatchAssignCodec() {
    proto::VehicleAssignData entries[3];
    entries[0] = { 10, 612, 1, 0 };
    entries[1] = { 20, 613, 2, 0 };
    entries[2] = { 30, 614, 3, 0 };

    uint8_t buf[256];
    size_t n = proto::EncodeBatchAssign(entries, 3, buf, sizeof(buf));
    CHECK(n == 1 + 3 * 8, "batch-size");

    proto::VehicleAssignData decoded[20];
    uint8_t count = 0;
    CHECK(proto::DecodeBatchAssign(buf, n, decoded, count), "batch-decode");
    CHECK(count == 3, "batch-count");
    CHECK(decoded[0].vehiclePoolId == 10, "batch-0-pool");
    CHECK(decoded[0].customModelId == 612, "batch-0-model");
    CHECK(decoded[0].generation == 1, "batch-0-gen");
    CHECK(decoded[1].vehiclePoolId == 20, "batch-1-pool");
    CHECK(decoded[2].vehiclePoolId == 30, "batch-2-pool");

    // Over limit
    CHECK(proto::EncodeBatchAssign(entries, proto::kMaxBatchEntries + 1, buf, sizeof(buf)) == 0, "batch-overlimit");
}

static void TestVehicleRespawnCodec() {
    uint8_t buf[16];
    size_t n = proto::EncodeVehicleRespawn(55, 42, buf, sizeof(buf));
    CHECK(n == proto::kVehicleRespawnSize, "respawn-size");

    uint16_t id; uint32_t gen;
    CHECK(proto::DecodeVehicleRespawn(buf, n, id, gen), "respawn-decode");
    CHECK(id == 55, "respawn-id");
    CHECK(gen == 42, "respawn-gen");
}

static void TestGatewayWrapper() {
    proto::VehicleAssignData a = { 42, 612, 1, 0 };
    uint8_t inner[proto::kVehicleAssignSize];
    size_t innerLen = proto::EncodeVehicleAssign(a, inner, sizeof(inner));
    CHECK(innerLen > 0, "gw-inner");

    uint8_t buf[64];
    size_t total = proto::EncodeVehiclesGateway(proto::VehicleOp::VehicleAssign, inner, innerLen, buf, sizeof(buf));
    CHECK(total > 0, "gw-total");

    // Verify gateway header
    using skr::protocol::skrx::GatewayHeader;
    using skr::protocol::skrx::DecodeGateway;
    GatewayHeader hdr;
    const uint8_t* payload = nullptr;
    CHECK(DecodeGateway(buf, total, hdr, payload), "gw-decode-hdr");
    CHECK(hdr.ns == skr::protocol::skrx::Namespace::Vehicles, "gw-ns");
    CHECK(hdr.version == proto::kVehiclesVersion, "gw-ver");
    CHECK(payload != nullptr, "gw-payload");
    CHECK(payload[0] == static_cast<uint8_t>(proto::VehicleOp::VehicleAssign), "gw-op");
}

static void TestValidation() {
    CHECK(proto::IsCustomModelId(612), "valid-612");
    CHECK(proto::IsCustomModelId(614), "valid-614");
    CHECK(!proto::IsCustomModelId(400), "invalid-400");
    CHECK(!proto::IsCustomModelId(611), "invalid-611");
    CHECK(!proto::IsCustomModelId(0), "invalid-0");
    CHECK(!proto::IsCustomModelId(-1), "invalid-neg");

    CHECK(proto::IsStockModelId(400), "stock-400");
    CHECK(proto::IsStockModelId(611), "stock-611");
    CHECK(!proto::IsStockModelId(612), "not-stock-612");
    CHECK(!proto::IsStockModelId(399), "not-stock-399");
}

// ═══════════════════════════════════════════════════════════════════
// B. VehicleManager tests (server-side logic, no network)
// ═══════════════════════════════════════════════════════════════════

static void TestModelRegistration() {
    VehicleManager mgr;

    CHECK(mgr.RegisterModel(612, 400, 400, "skr_xc60", "Volvo XC60", 4), "reg-612");
    CHECK(mgr.RegisterModel(613, 475, 475, "skr_m4gts", "BMW M4 GTS", 2), "reg-613");
    CHECK(mgr.RegisterModel(614, 560, 560, "skr_rs6", "Audi RS6", 4), "reg-614");
    CHECK(mgr.ModelCount() == 3, "reg-count");

    // Lookup
    auto* m = mgr.FindModel(612);
    CHECK(m != nullptr, "find-612");
    CHECK(m->proxyModelId == 400, "find-612-proxy");
    CHECK(strcmp(m->displayName, "Volvo XC60") == 0, "find-612-name");

    CHECK(mgr.IsCustomModel(612), "iscustom-612");
    CHECK(mgr.IsCustomModel(613), "iscustom-613");
    CHECK(!mgr.IsCustomModel(400), "iscustom-400");
    CHECK(!mgr.IsCustomModel(999), "iscustom-999");

    // Duplicate rejection
    CHECK(!mgr.RegisterModel(612, 400, 400, "dup", nullptr, 4), "reg-dup");

    // Invalid arguments
    CHECK(!mgr.RegisterModel(400, 400, 400, "stock", nullptr, 4), "reg-stock-id");
    CHECK(!mgr.RegisterModel(615, 999, 400, "bad-proxy", nullptr, 4), "reg-bad-proxy");
    CHECK(!mgr.RegisterModel(615, 400, 999, "bad-donor", nullptr, 4), "reg-bad-donor");
    CHECK(!mgr.RegisterModel(615, 400, 400, "", nullptr, 4), "reg-empty-name");
    CHECK(!mgr.RegisterModel(615, 400, 400, nullptr, nullptr, 4), "reg-null-name");

    // Unregister
    CHECK(mgr.UnregisterModel(613), "unreg-613");
    CHECK(mgr.ModelCount() == 2, "unreg-count");
    CHECK(!mgr.FindModel(613), "unreg-find");
    CHECK(!mgr.UnregisterModel(613), "unreg-twice");
}

static void TestVehicleLifecycle() {
    VehicleManager mgr;
    mgr.RegisterModel(612, 400, 400, "skr_xc60", "Volvo XC60", 4);
    mgr.RegisterModel(613, 475, 475, "skr_m4gts", nullptr, 2);

    // Track
    uint32_t gen1 = mgr.TrackVehicle(42, 612);
    CHECK(gen1 > 0, "track-42");
    CHECK(mgr.VehicleCount() == 1, "track-count");
    CHECK(mgr.GetCustomModel(42) == 612, "track-model");

    uint32_t gen2 = mgr.TrackVehicle(43, 613);
    CHECK(gen2 > 0, "track-43");
    CHECK(gen2 != gen1, "track-unique-gen");

    // Duplicate pool ID rejection
    CHECK(mgr.TrackVehicle(42, 612) == 0, "track-dup");

    // Unregistered model rejection
    CHECK(mgr.TrackVehicle(44, 999) == 0, "track-bad-model");

    // Find
    auto* slot = mgr.FindVehicle(42);
    CHECK(slot != nullptr, "find-42");
    CHECK(slot->customModelId == 612, "find-42-model");
    CHECK(slot->generation == gen1, "find-42-gen");
    CHECK(slot->alive, "find-42-alive");
    CHECK(!mgr.FindVehicle(999), "find-missing");

    // Respawn
    uint32_t gen3 = mgr.RespawnVehicle(42);
    CHECK(gen3 > gen1, "respawn-gen");
    CHECK(mgr.FindVehicle(42)->generation == gen3, "respawn-updated");
    CHECK(mgr.RespawnVehicle(999) == 0, "respawn-missing");

    // Untrack
    CHECK(mgr.UntrackVehicle(42), "untrack-42");
    CHECK(mgr.VehicleCount() == 1, "untrack-count");
    CHECK(!mgr.FindVehicle(42), "untrack-gone");
    CHECK(!mgr.UntrackVehicle(42), "untrack-twice");

    // Fill active
    proto::VehicleAssignData active[256];
    int count = mgr.FillActiveVehicles(active, 256);
    CHECK(count == 1, "fill-active-count");
    CHECK(active[0].vehiclePoolId == 43, "fill-active-id");
    CHECK(active[0].customModelId == 613, "fill-active-model");
}

static void TestPlayerBindings() {
    VehicleManager mgr;
    mgr.RegisterModel(612, 400, 400, "skr_xc60", nullptr, 4);
    uint32_t gen = mgr.TrackVehicle(42, 612);

    // Not ready initially
    CHECK(!mgr.IsPlayerReady(0, 42), "not-ready-init");

    // Player reports ready
    mgr.OnPlayerReady(0, 42, 612, gen, 0);
    CHECK(mgr.IsPlayerReady(0, 42), "ready-after-report");

    // Wrong generation rejected
    mgr.OnPlayerReady(1, 42, 612, gen - 1, 0);
    CHECK(!mgr.IsPlayerReady(1, 42), "wrong-gen");

    // Wrong model rejected
    mgr.OnPlayerReady(2, 42, 613, gen, 0);
    CHECK(!mgr.IsPlayerReady(2, 42), "wrong-model");

    // Asset failure status
    mgr.OnPlayerReady(3, 42, 612, gen, 1);
    CHECK(!mgr.IsPlayerReady(3, 42), "asset-fail");

    // Respawn invalidates binding
    mgr.RespawnVehicle(42);
    CHECK(!mgr.IsPlayerReady(0, 42), "respawn-invalidates");

    // Forget player
    mgr.OnPlayerReady(0, 42, 612, mgr.FindVehicle(42)->generation, 0);
    CHECK(mgr.IsPlayerReady(0, 42), "re-ready");
    mgr.ForgetPlayer(0);
    CHECK(!mgr.IsPlayerReady(0, 42), "forget-player");
}

static void TestFillRegistryEntries() {
    VehicleManager mgr;
    mgr.RegisterModel(612, 400, 400, "skr_xc60", nullptr, 4);
    mgr.RegisterModel(613, 475, 475, "skr_m4gts", nullptr, 2);

    proto::ModelRegistryEntry entries[32];
    int count = mgr.FillRegistryEntries(entries, 32);
    CHECK(count == 2, "fill-reg-count");
    CHECK(entries[0].customModelId == 612, "fill-reg-0");
    CHECK(entries[1].customModelId == 613, "fill-reg-1");

    // Round-trip through codec
    uint8_t wire[256];
    size_t n = proto::EncodeModelRegistry(entries, static_cast<uint8_t>(count), wire, sizeof(wire));
    CHECK(n > 0, "fill-reg-encode");

    proto::ModelRegistryEntry decoded[32];
    uint8_t decodedCount = 0;
    CHECK(proto::DecodeModelRegistry(wire, n, decoded, decodedCount), "fill-reg-roundtrip");
    CHECK(decodedCount == 2, "fill-reg-rt-count");
    CHECK(decoded[0].customModelId == 612, "fill-reg-rt-0");
}

static void TestCapacityLimits() {
    VehicleManager mgr;

    // Fill to capacity
    for (int i = 0; i < proto::kMaxCustomModels; ++i) {
        CHECK(mgr.RegisterModel(612 + i, 400, 400, "test", nullptr, 4), "cap-model");
    }
    CHECK(!mgr.RegisterModel(612 + proto::kMaxCustomModels, 400, 400, "overflow", nullptr, 4), "cap-model-overflow");

    // Vehicle capacity
    VehicleManager mgr2;
    mgr2.RegisterModel(612, 400, 400, "test", nullptr, 4);
    for (int i = 0; i < proto::kMaxCustomVehicles; ++i) {
        CHECK(mgr2.TrackVehicle(i, 612) > 0, "cap-vehicle");
    }
    CHECK(mgr2.TrackVehicle(proto::kMaxCustomVehicles, 612) == 0, "cap-vehicle-overflow");
}

static void TestReset() {
    VehicleManager mgr;
    mgr.RegisterModel(612, 400, 400, "test", nullptr, 4);
    mgr.TrackVehicle(42, 612);
    mgr.OnPlayerReady(0, 42, 612, mgr.FindVehicle(42)->generation, 0);

    mgr.Reset();
    CHECK(mgr.ModelCount() == 0, "reset-models");
    CHECK(mgr.VehicleCount() == 0, "reset-vehicles");
    CHECK(!mgr.IsPlayerReady(0, 42), "reset-bindings");
}

// ═══════════════════════════════════════════════════════════════════

int main() {
    // A. Codec
    TestVehicleAssignCodec();
    TestVehicleRevokeCodec();
    TestVehicleReadyCodec();
    TestModelRegistryCodec();
    TestBatchAssignCodec();
    TestVehicleRespawnCodec();
    TestGatewayWrapper();
    TestValidation();

    // B. Manager
    TestModelRegistration();
    TestVehicleLifecycle();
    TestPlayerBindings();
    TestFillRegistryEntries();
    TestCapacityLimits();
    TestReset();

    std::printf("\nSKRXVehiclesTests: %d checks, %d failures.\n", g_checks, g_fails);
    return g_fails ? EXIT_FAILURE : EXIT_SUCCESS;
}
