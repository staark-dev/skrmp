#include <cstdio>
#include <cstring>
#include <cstdlib>

#include "protocol/skrx_registry.h"
#include "protocol/skrx_gateway.h"

using namespace skr::protocol::skrx;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { if (cond) { ++g_pass; } else { ++g_fail; \
    fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg); } } while(0)

// ── Registry tests ──────────────────────────────────────────────────

static void TestRegistryBasic() {
    Registry reg;
    Entry e1{240, 0, Direction::Inbound, "client-state", 0, true, false, nullptr, nullptr};
    CHECK(reg.Register(e1), "register first entry");
    CHECK(reg.Count() == 1, "count is 1");
    CHECK(reg.Find(240, 0, Direction::Inbound) != nullptr, "find registered entry");
    CHECK(reg.Find(240, 0, Direction::Outbound) == nullptr, "miss on wrong direction");
    CHECK(reg.Find(241, 0, Direction::Inbound) == nullptr, "miss on wrong rpcId");
}

static void TestRegistryCollision() {
    Registry reg;
    Entry e1{240, 0, Direction::Inbound, "client-state", 0, true, false, nullptr, nullptr};
    CHECK(reg.Register(e1), "first register ok");
    CHECK(!reg.Register(e1), "duplicate rejected");
    CHECK(reg.Count() == 1, "count still 1");
}

static void TestRegistryUnregister() {
    Registry reg;
    Entry e1{240, 0, Direction::Inbound, "a", 0, false, false, nullptr, nullptr};
    Entry e2{241, 0, Direction::Outbound, "b", 0, false, false, nullptr, nullptr};
    reg.Register(e1);
    reg.Register(e2);
    CHECK(reg.Count() == 2, "two entries");
    CHECK(reg.Unregister(240, 0, Direction::Inbound), "unregister ok");
    CHECK(reg.Count() == 1, "count is 1 after unregister");
    CHECK(reg.Find(240, 0, Direction::Inbound) == nullptr, "removed entry gone");
    CHECK(reg.Find(241, 0, Direction::Outbound) != nullptr, "other entry still there");
    CHECK(!reg.Unregister(240, 0, Direction::Inbound), "double unregister fails");
}

static void TestRegistryCapacity() {
    Registry reg;
    for (int i = 0; i < Registry::kMaxEntries; ++i) {
        Entry e{static_cast<uint8_t>(i % 256), static_cast<uint8_t>(i / 256),
                Direction::Inbound, "x", 0, false, false, nullptr, nullptr};
        CHECK(reg.Register(e), "fill entry");
    }
    Entry extra{255, 255, Direction::Outbound, "overflow", 0, false, false, nullptr, nullptr};
    CHECK(!reg.Register(extra), "overflow rejected");
}

static void TestRegistryBuiltins() {
    Registry reg;
    CHECK(RegisterBuiltinEntries(reg), "builtin registration succeeds");
    CHECK(reg.Count() > 10, "at least 10 builtin entries");

    // Verify key entries exist
    CHECK(reg.Find(240, 0, Direction::Inbound) != nullptr, "client-state entry");
    CHECK(reg.Find(243, 0, Direction::Inbound) != nullptr, "radar-rect entry");
    CHECK(reg.Find(242, 0, Direction::Outbound) != nullptr, "radar-style entry");
    CHECK(reg.Find(241, 0, Direction::Outbound) != nullptr, "session-info entry");

    // Enhanced handshake entries
    CHECK(reg.Find(240, 1, Direction::Inbound) != nullptr, "enhanced-hello");
    CHECK(reg.Find(240, 3, Direction::Inbound) != nullptr, "enhanced-confirm");
    CHECK(reg.Find(241, 2, Direction::Outbound) != nullptr, "enhanced-offer");
    CHECK(reg.Find(241, 4, Direction::Outbound) != nullptr, "rejection");

    // API entries
    CHECK(reg.Find(240, 5, Direction::Inbound) != nullptr, "api-hello");
    CHECK(reg.Find(240, 8, Direction::Inbound) != nullptr, "api-reply");
    CHECK(reg.Find(240, 9, Direction::Inbound) != nullptr, "client-request");
    CHECK(reg.Find(241, 6, Direction::Outbound) != nullptr, "api-welcome");
    CHECK(reg.Find(241, 7, Direction::Outbound) != nullptr, "api-call");

    // Gateway entries
    CHECK(reg.Find(240, kGatewayStep, Direction::Inbound) != nullptr, "skrx-gateway-in");
    CHECK(reg.Find(241, kGatewayStep, Direction::Outbound) != nullptr, "skrx-gateway-out");

    // No double-registration
    Registry reg2;
    CHECK(RegisterBuiltinEntries(reg2), "second builtin registration succeeds");
    CHECK(reg2.Count() == reg.Count(), "same count both times");
}

// ── Gateway codec tests ─────────────────────────────────────────────

static void TestGatewayEncodeDecode() {
    uint8_t payload[] = {0x01, 0xAA, 0xBB, 0xCC};
    GatewayHeader hdr;
    hdr.ns = Namespace::Core;
    hdr.version = kCoreVersion;
    hdr.payloadLen = 4;

    uint8_t buf[64];
    size_t n = EncodeGateway(hdr, payload, buf, sizeof(buf));
    CHECK(n == kGatewayHeaderSize + 4, "encode size correct");
    CHECK(buf[0] == 0xE0, "marker");
    CHECK(buf[1] == kGatewayStep, "step");
    CHECK(buf[2] == 0, "namespace core");
    CHECK(buf[3] == kCoreVersion, "version");

    GatewayHeader decoded;
    const uint8_t* decodedPayload = nullptr;
    CHECK(DecodeGateway(buf, n, decoded, decodedPayload), "decode ok");
    CHECK(decoded.ns == Namespace::Core, "decoded namespace");
    CHECK(decoded.version == kCoreVersion, "decoded version");
    CHECK(decoded.payloadLen == 4, "decoded payload len");
    CHECK(decodedPayload && memcmp(decodedPayload, payload, 4) == 0, "decoded payload matches");
}

static void TestGatewayDecodeRejectsShort() {
    uint8_t buf[3] = {0xE0, kGatewayStep, 0};
    GatewayHeader hdr;
    const uint8_t* payload = nullptr;
    CHECK(!DecodeGateway(buf, 3, hdr, payload), "reject short message");
}

static void TestGatewayDecodeRejectsBadMarker() {
    uint8_t buf[8] = {0x00, kGatewayStep, 0, 1, 0, 0};
    GatewayHeader hdr;
    const uint8_t* payload = nullptr;
    CHECK(!DecodeGateway(buf, 6, hdr, payload), "reject bad marker");
}

static void TestGatewayDecodeRejectsLengthMismatch() {
    uint8_t buf[10] = {0xE0, kGatewayStep, 0, 1, 4, 0, 0xAA, 0xBB, 0xCC, 0xDD};
    GatewayHeader hdr;
    const uint8_t* payload = nullptr;
    CHECK(DecodeGateway(buf, 10, hdr, payload), "exact length ok");
    CHECK(!DecodeGateway(buf, 9, hdr, payload), "reject truncated");
    CHECK(!DecodeGateway(buf, 8, hdr, payload), "reject shorter");
}

static void TestGatewayEmptyPayload() {
    GatewayHeader hdr;
    hdr.ns = Namespace::Diagnostics;
    hdr.version = 1;
    hdr.payloadLen = 0;

    uint8_t buf[16];
    size_t n = EncodeGateway(hdr, nullptr, buf, sizeof(buf));
    CHECK(n == kGatewayHeaderSize, "header-only encode");

    GatewayHeader decoded;
    const uint8_t* payload = nullptr;
    CHECK(DecodeGateway(buf, n, decoded, payload), "decode header-only");
    CHECK(decoded.payloadLen == 0, "zero payload");
    CHECK(payload == nullptr, "no payload pointer");
}

// ── Ping/Pong codec tests ───────────────────────────────────────────

static void TestPingEncodeDecode() {
    CorePing ping;
    ping.clientTimestamp = 0xDEADBEEF;

    uint8_t buf[4];
    CHECK(EncodePing(ping, buf, sizeof(buf)) == 4, "ping encode size");

    CorePing decoded;
    CHECK(DecodePing(buf, 4, decoded), "ping decode ok");
    CHECK(decoded.clientTimestamp == 0xDEADBEEF, "ping timestamp match");
}

static void TestPongEncodeDecode() {
    CorePong pong;
    pong.echoedTimestamp = 12345;
    pong.serverTimestamp = 67890;

    uint8_t buf[8];
    CHECK(EncodePong(pong, buf, sizeof(buf)) == 8, "pong encode size");

    CorePong decoded;
    CHECK(DecodePong(buf, 8, decoded), "pong decode ok");
    CHECK(decoded.echoedTimestamp == 12345, "pong echo match");
    CHECK(decoded.serverTimestamp == 67890, "pong server match");
}

static void TestPingDecodeRejectsBadSize() {
    uint8_t buf[3] = {0, 0, 0};
    CorePing ping;
    CHECK(!DecodePing(buf, 3, ping), "reject 3 bytes");
    uint8_t buf5[5] = {0, 0, 0, 0, 0};
    CHECK(!DecodePing(buf5, 5, ping), "reject 5 bytes");
}

static void TestPongDecodeRejectsBadSize() {
    uint8_t buf[7] = {};
    CorePong pong;
    CHECK(!DecodePong(buf, 7, pong), "reject 7 bytes");
}

static void TestFullGatewayPingRoundtrip() {
    CorePing ping;
    ping.clientTimestamp = 42000;

    uint8_t buf[64];
    size_t n = EncodeGatewayPing(ping, buf, sizeof(buf));
    CHECK(n > 0, "gateway ping encode");

    CHECK(IsGatewayMessage(buf, n), "is gateway message");

    GatewayHeader hdr;
    const uint8_t* payload = nullptr;
    CHECK(DecodeGateway(buf, n, hdr, payload), "decode gateway");
    CHECK(hdr.ns == Namespace::Core, "namespace core");
    CHECK(hdr.version == kCoreVersion, "core version");

    // Inner: u8 op + u32 timestamp
    CHECK(hdr.payloadLen == 5, "inner payload = op + 4 bytes");
    CHECK(payload[0] == static_cast<uint8_t>(CoreOp::Ping), "op is Ping");

    CorePing decoded;
    CHECK(DecodePing(payload + 1, hdr.payloadLen - 1, decoded), "decode inner ping");
    CHECK(decoded.clientTimestamp == 42000, "timestamp roundtrip");
}

static void TestFullGatewayPongRoundtrip() {
    CorePong pong;
    pong.echoedTimestamp = 42000;
    pong.serverTimestamp = 42005;

    uint8_t buf[64];
    size_t n = EncodeGatewayPong(pong, buf, sizeof(buf));
    CHECK(n > 0, "gateway pong encode");

    GatewayHeader hdr;
    const uint8_t* payload = nullptr;
    CHECK(DecodeGateway(buf, n, hdr, payload), "decode gateway");

    // Inner: u8 op + u32 echo + u32 server
    CHECK(hdr.payloadLen == 9, "inner payload = op + 8 bytes");
    CHECK(payload[0] == static_cast<uint8_t>(CoreOp::Pong), "op is Pong");

    CorePong decoded;
    CHECK(DecodePong(payload + 1, hdr.payloadLen - 1, decoded), "decode inner pong");
    CHECK(decoded.echoedTimestamp == 42000, "echo roundtrip");
    CHECK(decoded.serverTimestamp == 42005, "server roundtrip");
}

static void TestIsGatewayMessage() {
    uint8_t good[] = {0xE0, kGatewayStep, 0, 1, 0, 0};
    CHECK(IsGatewayMessage(good, sizeof(good)), "valid gateway");

    uint8_t bad1[] = {0xE0, 5, 0, 1, 0, 0};
    CHECK(!IsGatewayMessage(bad1, sizeof(bad1)), "step 5 is not gateway");

    uint8_t bad2[] = {0x00, kGatewayStep, 0, 1, 0, 0};
    CHECK(!IsGatewayMessage(bad2, sizeof(bad2)), "bad marker");

    CHECK(!IsGatewayMessage(nullptr, 0), "null");
    CHECK(!IsGatewayMessage(good, 1), "too short");
}

static void TestKnownNamespace() {
    CHECK(IsKnownNamespace(0), "core");
    CHECK(IsKnownNamespace(1), "vehicles");
    CHECK(IsKnownNamespace(2), "assets");
    CHECK(IsKnownNamespace(3), "hud");
    CHECK(IsKnownNamespace(4), "gps");
    CHECK(IsKnownNamespace(5), "phone namespace registered");
    CHECK(IsKnownNamespace(6), "ui");
    CHECK(IsKnownNamespace(7), "diagnostics");
    CHECK(!IsKnownNamespace(8), "unknown");
    CHECK(!IsKnownNamespace(255), "unknown 255");
}

// ── Main ────────────────────────────────────────────────────────────

int main() {
    // Registry
    TestRegistryBasic();
    TestRegistryCollision();
    TestRegistryUnregister();
    TestRegistryCapacity();
    TestRegistryBuiltins();

    // Gateway codec
    TestGatewayEncodeDecode();
    TestGatewayDecodeRejectsShort();
    TestGatewayDecodeRejectsBadMarker();
    TestGatewayDecodeRejectsLengthMismatch();
    TestGatewayEmptyPayload();

    // Ping/Pong codec
    TestPingEncodeDecode();
    TestPongEncodeDecode();
    TestPingDecodeRejectsBadSize();
    TestPongDecodeRejectsBadSize();
    TestFullGatewayPingRoundtrip();
    TestFullGatewayPongRoundtrip();

    // Utility
    TestIsGatewayMessage();
    TestKnownNamespace();

    printf("%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
