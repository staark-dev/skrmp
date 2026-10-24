// SKR Native Runtime API — client session (TASK API-001). See native_api.h.
#include "native-api/native_api.h"
#include "native-api/native_api_ui.h"
#include "game-state/game_state.h"
#include "game-state/radar_hud.h"
#include "protocol/native_api.h"
#include "phone/phone_net.h"
#include "protocol/skrx_phone.h"
#include "skrx/skrx_client.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace skr::native_api {
namespace {

namespace api = protocol::api;

// Capabilities this client build implements.
constexpr uint32_t kClientCapabilities = api::CapNotify | api::CapConfirm | api::CapHudValue | api::CapPhone | api::CapGateway | api::CapPhoneSetup | api::CapPhoneGateway;
constexpr int kMaxLogLinesPerSession = 200;

enum class State : uint8_t {
    Idle,          // no Enhanced session
    HelloPending,  // Enhanced confirmed; Hello goes out on the next Tick
    HelloSent,     // waiting for Welcome
    Ready,
    Unavailable,   // server has no API (old Runtime) or refused our versions
};

State    g_state = State::Idle;
uint32_t g_enhancedToken = 0;
uint64_t g_welcomeDeadline = 0;
uint32_t g_sessionId = 0;
uint32_t g_capabilities = 0;
uint32_t g_invalid = 0;
int      g_logLines = 0;

// ── Log (SKR\native_api.log next to gta_sa.exe), bounded per session ──
void Log(const char* fmt, ...) {
    if (g_logLines >= kMaxLogLinesPerSession) return;
    ++g_logLines;
    char path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    char* slash = strrchr(path, '\\');
    if (!slash) return;
    snprintf(slash + 1, MAX_PATH - (slash + 1 - path), "SKR\\native_api.log");
    FILE* f = fopen(path, "a");
    if (!f) return;
    fprintf(f, "[%lu] ", GetTickCount());
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fprintf(f, "\n");
    fclose(f);
}

const char* StateName(State s) {
    switch (s) {
        case State::Idle: return "idle";
        case State::HelloPending: return "hello-pending";
        case State::HelloSent: return "hello-sent";
        case State::Ready: return "ready";
        default: return "unavailable";
    }
}

void Invalid(const char* reason) {
    ++g_invalid;
    Log("event=invalid reason=%s state=%s count=%u", reason, StateName(g_state), g_invalid);
}

bool SendReply(uint32_t callId, api::Op op, api::ReplyResult result) {
    api::Reply r;
    r.sessionId = g_sessionId;
    r.callId = callId;
    r.op = op;
    r.result = result;
    uint8_t out[api::kMaxReplySize];
    const size_t n = api::EncodeReply(r, out, sizeof(out));
    const bool sent = n && skr::game_state::SendEnhancedEnvelope(out, n);
    Log("event=reply call=%u op=%u result=%u %s", callId, static_cast<unsigned>(op),
        static_cast<unsigned>(result), sent ? "sent" : "NOT sent");
    return sent;
}

void OnWelcome(const uint8_t* data, size_t len) {
    if (g_state != State::HelloSent) { Invalid("unexpected welcome"); return; }
    api::Welcome w;
    if (!api::DecodeWelcome(data, len, w)) { Invalid("malformed welcome"); return; }
    if (w.status != api::WelcomeStatus::Accepted) {
        g_state = State::Unavailable;
        Log("event=welcome-refused reason=version client=%u-%u", api::kVersionMin, api::kVersionMax);
        return;
    }
    if (w.capabilities & ~kClientCapabilities) { Invalid("welcome grants capabilities we did not offer"); return; }
    if (protocol::skrx::phone::NormalizeCapabilities(w.capabilities)!=w.capabilities) { Invalid("incoherent phone capabilities"); return; }
    g_sessionId = w.sessionId;
    g_capabilities = w.capabilities;
    g_state = State::Ready;
    Log("event=ready version=%u session=%08x caps=0x%x", w.version, w.sessionId, w.capabilities);
    if (w.capabilities & api::CapPhone)
        skr::phone::net::OnPhoneReady(w.sessionId, (w.capabilities & api::CapPhoneSetup) != 0,
            protocol::skrx::phone::SelectTransport(w.capabilities)==protocol::skrx::phone::Transport::GatewayV1);

    // SKR-X: enable gateway only when the server negotiated CapGateway
    skr::skrx::SetGatewayReady((w.capabilities & api::CapGateway) != 0);
    if (w.capabilities & api::CapGateway)
        skr::skrx::SendPing();
}

void OnCall(const uint8_t* data, size_t len) {
    if (g_state != State::Ready) { Invalid("call before ready"); return; }
    api::Call call;
    if (!api::DecodeCall(data, len, call)) { Invalid("malformed call"); return; }
    if (call.sessionId != g_sessionId) { Invalid("stale session"); return; }
    if (!(g_capabilities & api::CapabilityFor(call.op))) {
        if (call.op == api::Op::Confirm) SendReply(call.callId, call.op, api::ReplyResult::Unsupported);
        Invalid("op not negotiated");
        return;
    }
    switch (call.op) {
        case api::Op::Notify: {
            api::Notify m;
            if (!api::DecodeNotify(call.payload, call.payloadLen, m)) { Invalid("bad notify"); return; }
            ui::PushNotification(m.kind, m.durationSec, m.text);
            Log("event=notify call=%u kind=%u", call.callId, static_cast<unsigned>(m.kind));
            break;
        }
        case api::Op::Confirm: {
            api::Confirm m;
            if (!api::DecodeConfirm(call.payload, call.payloadLen, m)) {
                SendReply(call.callId, call.op, api::ReplyResult::Invalid);
                Invalid("bad confirm");
                return;
            }
            if (ui::ConfirmActive()) {
                SendReply(call.callId, call.op, api::ReplyResult::Busy);
                return;
            }
            ui::OpenConfirm(call.callId, m.title, m.message, m.timeoutSec);
            Log("event=confirm-open call=%u timeout=%us", call.callId, m.timeoutSec);
            break;
        }
        case api::Op::CancelConfirm: {
            uint32_t target = 0;
            if (!api::DecodeCancelConfirm(call.payload, call.payloadLen, target)) { Invalid("bad cancel"); return; }
            if (ui::ConfirmActive() && ui::ConfirmCallId() == target) {
                ui::CloseConfirm();
                Log("event=confirm-withdrawn call=%u", target);
            }
            break;
        }
        case api::Op::HudSet: {
            api::HudSet m;
            if (!api::DecodeHudSet(call.payload, call.payloadLen, m)) { Invalid("bad hudset"); return; }
            skr::radar_hud::SetServerVitalsNumbers(m.value);
            Log("event=hud-set component=vitals_numbers value=%u", m.value);
            break;
        }
        default:
            if (api::IsPhoneOp(static_cast<uint8_t>(call.op))) {
                if (!skr::phone::net::OnPhoneCall(static_cast<uint8_t>(call.op), call.callId,
                                                   call.payload, call.payloadLen))
                    Invalid("unhandled phone op");
            }
            break;
    }
}

} // namespace

void OnEnhancedConfirmed(uint32_t enhancedToken) {
    if (g_state != State::Idle || !enhancedToken) return;
    g_enhancedToken = enhancedToken;
    g_state = State::HelloPending;
    g_welcomeDeadline = GetTickCount64() + api::kWelcomeTimeoutMs;   // bounds send retries too
}

void OnServerMessage(const uint8_t* data, size_t len) {
    if (!data || len < 2) return;
    if (g_state == State::Idle || g_state == State::Unavailable) return;
    switch (static_cast<api::Step>(data[1])) {
        case api::Step::Welcome: OnWelcome(data, len); break;
        case api::Step::Call:    OnCall(data, len); break;
        default:                 Invalid("client-to-server step from server"); break;
    }
}

void Tick() {
    const uint64_t now = GetTickCount64();
    if (g_state == State::HelloPending) {
        api::Hello h;
        h.enhancedToken = g_enhancedToken;
        h.capabilities = kClientCapabilities;
        uint8_t out[api::kHelloSize];
        const size_t n = api::EncodeHello(h, out, sizeof(out));
        if (n && skr::game_state::SendEnhancedEnvelope(out, n)) {
            g_state = State::HelloSent;
            g_welcomeDeadline = now + api::kWelcomeTimeoutMs;
            Log("event=hello-sent versions=%u-%u caps=0x%x", h.versionMin, h.versionMax, h.capabilities);
        } else if (now >= g_welcomeDeadline) {
            g_state = State::Unavailable;
            Log("event=unavailable reason=hello-send-failed");
        }
        return;   // otherwise retried next frame
    }
    if (g_state == State::HelloSent && now >= g_welcomeDeadline) {
        g_state = State::Unavailable;
        Log("event=unavailable reason=no-welcome (server without Native API)");
        return;
    }
    if (g_state != State::Ready) return;

    ui::Tick(now);
    skr::phone::net::Tick(now);          // paced phone requests + timeouts (no-op without CapPhone)
    skr::skrx::Tick();
    uint32_t callId = 0;
    api::ReplyResult result;
    if (ui::TakeConfirmAnswer(callId, result)) SendReply(callId, api::Op::Confirm, result);
}

void Reset() {
    if (g_state != State::Idle) Log("event=reset from=%s invalid=%u", StateName(g_state), g_invalid);
    g_state = State::Idle;
    g_enhancedToken = 0;
    g_welcomeDeadline = 0;
    g_sessionId = 0;
    g_capabilities = 0;
    g_invalid = 0;
    g_logLines = 0;
    ui::Reset();
    skr::radar_hud::SetServerVitalsNumbers(-1);
    skr::phone::net::Reset();
}

void Draw() {
    // Notifications and confirm dialog are now rendered in the ImGui pass
    // (pause_renderer.cpp → ui::DrawImGui). This stub is kept for the header
    // contract but is a no-op: the GTA 2D HUD pass no longer draws them.
}

bool OnWndProc(unsigned int msg, uintptr_t wParam, intptr_t lParam) {
    return g_state == State::Ready && ui::OnWndProc(msg, wParam, lParam);
}

bool IsReady() { return g_state == State::Ready; }

} // namespace skr::native_api
