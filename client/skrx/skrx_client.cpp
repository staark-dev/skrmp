#include "skrx_client.h"
#include "skrx/skrx_vehicles_client.h"
#include "phone/phone_net.h"
#include "protocol/skrx_gateway.h"
#include "protocol/skrx_registry.h"
#include "protocol/client_state.h"
#include "protocol/session_info.h"
#include "protocol/radar_style.h"
#include "protocol/radar_rect.h"

#include <windows.h>
#include <cstring>
#include <cstdio>
#include <cstdarg>

// SKR-X client implementation: registry, gateway dispatch, CORE Ping/Pong.
//
// Logging: entries are buffered in a ring and flushed to disk from Tick()
// or Reset(), never from a network callback.

namespace skr::skrx {

// ── Ring-buffer diagnostic log (non-blocking in callbacks) ─────────
namespace {

struct LogEntry {
    uint32_t tick;
    char msg[120];
};

constexpr int kLogRingCapacity = 64;
LogEntry g_logRing[kLogRingCapacity];
int g_logHead = 0;
int g_logCount = 0;
int g_logFlushed = 0;
constexpr int kMaxFlushPerSession = 400;

void Log(const char* fmt, ...) {
    auto& e = g_logRing[g_logHead];
    e.tick = GetTickCount();
    va_list args;
    va_start(args, fmt);
    vsnprintf(e.msg, sizeof(e.msg), fmt, args);
    va_end(args);
    g_logHead = (g_logHead + 1) % kLogRingCapacity;
    if (g_logCount < kLogRingCapacity) ++g_logCount;
}

void FlushLog() {
    if (!g_logCount || g_logFlushed >= kMaxFlushPerSession) return;
    char path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    char* slash = strrchr(path, '\\');
    if (!slash) return;
    snprintf(slash + 1, MAX_PATH - (slash + 1 - path), "SKR\\skrx.log");
    FILE* f = fopen(path, "a");
    if (!f) return;
    const int start = (g_logHead - g_logCount + kLogRingCapacity) % kLogRingCapacity;
    for (int i = 0; i < g_logCount && g_logFlushed < kMaxFlushPerSession; ++i, ++g_logFlushed) {
        const auto& e = g_logRing[(start + i) % kLogRingCapacity];
        fprintf(f, "[%lu] %s\n", e.tick, e.msg);
    }
    fclose(f);
    g_logCount = 0;
}

} // namespace

using namespace skr::protocol;
using namespace skr::protocol::skrx;

static Registry g_registry;
static SkrXState g_state;

// ── RakClient send helper ───────────────────────────────────────────
static void* s_rakClient = nullptr;
static uint8_t* s_rakVtbl = nullptr;

using RakRpcFn = bool(__thiscall*)(void*, int*, void*, int, int, char, bool);
static constexpr size_t kRakRpcVtableOffset = 0x64;

void SetRakClient(void* rakClient) {
    s_rakClient = rakClient;
    if (rakClient) s_rakVtbl = *reinterpret_cast<uint8_t**>(rakClient);
    else s_rakVtbl = nullptr;
}

struct MiniBitStream {
    int numBitsAllocated;
    int numBitsUsed;
    int readOffset;
    unsigned char* data;
    bool copyData;
    unsigned char stackBuf[256];
};

static bool SendOnRpc240(const uint8_t* data, size_t len) {
    if (!s_rakClient || !s_rakVtbl || len > 256) return false;
    auto rpc = *reinterpret_cast<RakRpcFn*>(s_rakVtbl + kRakRpcVtableOffset);
    int rpcId = kRpcClientState;
    MiniBitStream bs{};
    bs.numBitsAllocated = static_cast<int>(len * 8);
    bs.numBitsUsed = static_cast<int>(len * 8);
    bs.readOffset = 0;
    memcpy(bs.stackBuf, data, len);
    bs.data = bs.stackBuf;
    bs.copyData = true;
    return rpc(s_rakClient, &rpcId, &bs, 1, 9, 0, false);
}

// ── Forward declarations ────────────────────────────────────────────
static void OnCoreMessage(const GatewayHeader& hdr, const uint8_t* payload, size_t payloadLen);
static void OnCorePong(const uint8_t* data, size_t len);

// ── Public API ──────────────────────────────────────────────────────

static bool RegisterClientEntries(Registry& reg) {
    const Entry entries[] = {
        {kRpcClientState, 0, Direction::Outbound, "client-state", 0, true, false, nullptr, nullptr},
        {kRpcSessionInfo, 0, Direction::Inbound,  "session-info", 0, false, false, nullptr, nullptr},
        {kRpcRadarStyle,  0, Direction::Inbound,  "radar-style",  0, true, false, nullptr, nullptr},
        {kRpcRadarRect,   0, Direction::Outbound, "radar-rect",   0, true, false, nullptr, nullptr},
        {kRpcClientState, 1, Direction::Outbound, "enhanced-hello",   0, false, false, nullptr, nullptr},
        {kRpcSessionInfo, 2, Direction::Inbound,  "enhanced-offer",   0, false, false, nullptr, nullptr},
        {kRpcClientState, 3, Direction::Outbound, "enhanced-confirm", 0, false, false, nullptr, nullptr},
        {kRpcSessionInfo, 4, Direction::Inbound,  "rejection",        0, false, false, nullptr, nullptr},
        {kRpcClientState, 5, Direction::Outbound, "api-hello",    0, true, false, nullptr, nullptr},
        {kRpcSessionInfo, 6, Direction::Inbound,  "api-welcome",  0, true, false, nullptr, nullptr},
        {kRpcSessionInfo, 7, Direction::Inbound,  "api-call",     0, true, true,  nullptr, nullptr},
        {kRpcClientState, 8, Direction::Outbound, "api-reply",    0, true, true,  nullptr, nullptr},
        {kRpcClientState, 9, Direction::Outbound, "client-request", 0, true, true, nullptr, nullptr},
        {kRpcClientState, kGatewayStep, Direction::Outbound, "skrx-gateway-out", 0, true, true, nullptr, nullptr},
        {kRpcSessionInfo, kGatewayStep, Direction::Inbound,  "skrx-gateway-in",  0, true, true, nullptr, nullptr},
    };
    for (const auto& e : entries)
        if (!reg.Register(e)) return false;
    return true;
}

bool Init() {
    g_registry.Clear();
    g_state = {};
    return RegisterClientEntries(g_registry);
}

void Reset() {
    if (g_state.gatewayReady)
        Log("event=reset rtt=%u pings=%u pongs=%u timeouts=%u unsolicited=%u stale=%u",
            g_state.lastRttMs, g_state.pingCount, g_state.pongCount,
            g_state.timeoutCount, g_state.unsolicitedPongCount, g_state.stalePongCount);
    FlushLog();
    skr::skrx::vehicles::Reset();   // assignments and acks belong to the old session
    g_state = {};
    s_rakClient = nullptr;
    s_rakVtbl = nullptr;
    g_logFlushed = 0;
}

void SetGatewayReady(bool ready) {
    const bool was = g_state.gatewayReady;
    g_state.gatewayReady = ready;
    if (ready && !was) Log("event=gateway-ready");
    if (!ready && was) Log("event=gateway-disabled");
}

bool IsGatewayReady() { return g_state.gatewayReady; }

const Registry& GetRegistry() { return g_registry; }
SkrXState& GetState() { return g_state; }

void Tick() {
    if (g_state.pendingPing && g_state.gatewayReady) {
        const uint32_t now = GetTickCount();
        if (now - g_state.lastPingSentMs > kPingTimeoutMs) {
            ++g_state.timeoutCount;
            g_state.pendingPing = false;
            Log("event=ping-timeout ts=%u elapsed=%u timeouts=%u",
                g_state.lastPingSentMs, now - g_state.lastPingSentMs, g_state.timeoutCount);
        }
    }
    skr::skrx::vehicles::Tick();   // VehicleReady acks queued by the CVehiclePool hook
    FlushLog();
}

bool SendGateway(const uint8_t* data, size_t len) {
    if (!g_state.gatewayReady || !data || !len) return false;
    return SendOnRpc240(data, len);
}

void OnGatewayMessage(const uint8_t* data, size_t len) {
    if (!g_state.gatewayReady) {
        Log("event=gateway-rejected-no-cap len=%zu", len);
        return;
    }

    GatewayHeader hdr;
    const uint8_t* payload = nullptr;
    if (!DecodeGateway(data, len, hdr, payload)) {
        Log("event=gateway-decode-fail len=%zu", len);
        return;
    }
    if (!IsKnownNamespace(static_cast<uint8_t>(hdr.ns))) {
        Log("event=gateway-unknown-ns ns=%u", static_cast<unsigned>(hdr.ns));
        return;
    }
    // PHONE adds no per-message disk-log traffic to the existing game-tick logger.
    if (hdr.ns!=Namespace::Phone)
        Log("event=gateway-recv ns=%u ver=%u len=%u", static_cast<unsigned>(hdr.ns), hdr.version, hdr.payloadLen);

    switch (hdr.ns) {
    case Namespace::Core:
        OnCoreMessage(hdr, payload, hdr.payloadLen);
        break;
    case Namespace::Phone:
        skr::phone::net::OnPhoneGatewayMessage(hdr,payload,hdr.payloadLen);
        break;
    case Namespace::Vehicles:
        skr::skrx::vehicles::OnVehiclesMessage(hdr, payload, hdr.payloadLen);
        break;
    default:
        Log("event=gateway-ns-unhandled ns=%u", static_cast<unsigned>(hdr.ns));
        break;
    }
}

bool SendPing() {
    if (!g_state.gatewayReady) {
        Log("event=ping-blocked reason=no-gateway-cap");
        return false;
    }
    if (g_state.pendingPing) {
        Log("event=ping-blocked reason=pending ts=%u", g_state.lastPingSentMs);
        return false;
    }

    CorePing ping;
    ping.clientTimestamp = GetTickCount();

    uint8_t inner[8];
    inner[0] = static_cast<uint8_t>(CoreOp::Ping);
    uint8_t pingBuf[4];
    const size_t pingLen = EncodePing(ping, pingBuf, sizeof(pingBuf));
    if (!pingLen) return false;
    memcpy(inner + 1, pingBuf, pingLen);

    GatewayHeader hdr;
    hdr.ns = Namespace::Core;
    hdr.version = kCoreVersion;
    hdr.payloadLen = static_cast<uint16_t>(1 + pingLen);

    uint8_t buf[64];
    const size_t total = EncodeGateway(hdr, inner, buf, sizeof(buf));
    if (!total) return false;

    const bool ok = SendOnRpc240(buf, total);
    if (ok) {
        g_state.pendingPing = true;
        g_state.lastPingSentMs = ping.clientTimestamp;
        g_state.pendingEcho = ping.clientTimestamp;
        ++g_state.pingCount;
    }
    Log("event=ping-%s ts=%u count=%u", ok ? "sent" : "send-failed", ping.clientTimestamp, g_state.pingCount);
    return ok;
}

// ── CORE namespace dispatch ─────────────────────────────────────────

static void OnCoreMessage(const GatewayHeader& hdr, const uint8_t* payload, size_t payloadLen) {
    if (hdr.version != kCoreVersion || payloadLen < 1) {
        Log("event=core-version-mismatch ver=%u expected=%u", hdr.version, kCoreVersion);
        return;
    }

    const auto op = static_cast<CoreOp>(payload[0]);
    const uint8_t* opData = payload + 1;
    const size_t opLen = payloadLen - 1;

    switch (op) {
    case CoreOp::Pong:
        OnCorePong(opData, opLen);
        break;
    default:
        Log("event=core-unknown-op op=%u", static_cast<unsigned>(payload[0]));
        break;
    }
}

static void OnCorePong(const uint8_t* data, size_t len) {
    CorePong pong;
    if (!DecodePong(data, len, pong)) {
        Log("event=pong-decode-fail len=%zu", len);
        return;
    }

    if (!g_state.pendingPing) {
        ++g_state.unsolicitedPongCount;
        Log("event=pong-unsolicited echo=%u server=%u unsolicited=%u",
            pong.echoedTimestamp, pong.serverTimestamp, g_state.unsolicitedPongCount);
        return;
    }

    if (pong.echoedTimestamp != g_state.pendingEcho) {
        ++g_state.stalePongCount;
        Log("event=pong-stale echo=%u expected=%u stale=%u",
            pong.echoedTimestamp, g_state.pendingEcho, g_state.stalePongCount);
        return;
    }

    const uint32_t now = GetTickCount();
    g_state.lastRttMs = (now >= g_state.lastPingSentMs) ? (now - g_state.lastPingSentMs) : 0;
    g_state.pendingPing = false;
    ++g_state.pongCount;

    Log("event=pong-ok echo=%u server=%u rtt=%u pongs=%u",
        pong.echoedTimestamp, pong.serverTimestamp, g_state.lastRttMs, g_state.pongCount);
}

} // namespace skr::skrx
