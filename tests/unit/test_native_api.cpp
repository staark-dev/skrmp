// Unit tests for shared/protocol/native_api.h (TASK API-001).
#include "protocol/native_api.h"

#include <cstdlib>
#include <cstring>
#include <iostream>

namespace {
int checks = 0;

void Check(bool condition, const char* message) {
    ++checks;
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}
} // namespace

int main() {
    using namespace skr::protocol;
    using namespace skr::protocol::api;

    // ── Steps do not collide with the Enhanced handshake / rejection ──
    Check(!IsApiStep(static_cast<uint8_t>(EnhancedStep::Hello)), "step 1 is handshake");
    Check(!IsApiStep(static_cast<uint8_t>(EnhancedStep::Offer)), "step 2 is handshake");
    Check(!IsApiStep(static_cast<uint8_t>(EnhancedStep::Confirm)), "step 3 is handshake");
    Check(!IsApiStep(4), "step 4 is rejection");
    Check(IsApiStep(5) && IsApiStep(8) && IsApiStep(9) && !IsApiStep(10), "api steps are 5..9");
    // Negotiate() only accepts exactly 64-bit messages: no API message may be 8 bytes.
    Check(kHelloSize != 8 && kWelcomeSize != 8 && kCallHeaderSize > 8 && kReplyHeaderSize > 8,
          "api messages never look like an 8-byte handshake message");

    // ── Version selection ──
    Check(ChooseVersion(1, 1) == 1, "v1 only");
    Check(ChooseVersion(1, 9) == 1, "future client range picks our max");
    Check(ChooseVersion(2, 9) == 0, "no overlap");
    Check(ChooseVersion(0, 1) == 0, "min 0 invalid");
    Check(ChooseVersion(3, 2) == 0, "inverted range invalid");

    uint8_t buf[512];

    // ── Hello ──
    {
        Hello h; h.enhancedToken = 0xA1B2C3D4u; h.capabilities = kKnownCapabilities | 0x80000000u;
        const size_t n = EncodeHello(h, buf, sizeof(buf));
        Check(n == kHelloSize, "hello size");
        Check(buf[0] == kEnhancedMarker && buf[1] == 5, "hello envelope");
        Check(buf[4] == 0xD4 && buf[7] == 0xA1, "hello token little-endian");
        Hello d;
        Check(DecodeHello(buf, n, d), "hello roundtrip");
        Check(d.enhancedToken == h.enhancedToken && d.capabilities == h.capabilities, "hello fields");
        Check(!DecodeHello(buf, n - 1, d), "hello short");
        Check(!DecodeHello(buf, n + 1 > sizeof(buf) ? n : n + 1, d), "hello long");
        Hello z = h; z.enhancedToken = 0;
        EncodeHello(z, buf, sizeof(buf));
        Check(!DecodeHello(buf, kHelloSize, d), "hello zero token rejected");
        Check(EncodeHello(h, buf, kHelloSize - 1) == 0, "hello encode respects capacity");
    }

    // ── Welcome ──
    {
        Welcome w; w.version = 1; w.sessionId = 77; w.capabilities = CapConfirm | CapNotify;
        const size_t n = EncodeWelcome(w, buf, sizeof(buf));
        Check(n == kWelcomeSize, "welcome size");
        Welcome d;
        Check(DecodeWelcome(buf, n, d) && d.sessionId == 77 && d.version == 1, "welcome roundtrip");
        buf[14] = 1;   // reserved
        Check(!DecodeWelcome(buf, n, d), "welcome reserved must be 0");
        Welcome bad = w; bad.capabilities = 0x100;
        EncodeWelcome(bad, buf, sizeof(buf));
        Check(!DecodeWelcome(buf, n, d), "welcome unknown caps rejected by client");
        bad = w; bad.sessionId = 0;
        EncodeWelcome(bad, buf, sizeof(buf));
        Check(!DecodeWelcome(buf, n, d), "welcome zero session rejected");
        Welcome refuse; refuse.status = WelcomeStatus::VersionUnsupported;
        EncodeWelcome(refuse, buf, sizeof(buf));
        Check(DecodeWelcome(buf, n, d) && d.status == WelcomeStatus::VersionUnsupported,
              "refusal decodes without session");
    }

    // ── Confirm call roundtrip ──
    uint8_t payload[kMaxCallPayload];
    {
        Confirm c; c.timeoutSec = 30;
        strcpy(c.title, "Teleport"); c.titleLen = 8;
        strcpy(c.message, "Go to Grove Street?"); c.messageLen = 19;
        const size_t pn = EncodeConfirm(c, payload, sizeof(payload));
        Check(pn == 1 + 1 + 8 + 1 + 19, "confirm payload size");
        const size_t n = EncodeCall(9, 3, Op::Confirm, payload, pn, buf, sizeof(buf));
        Check(n == kCallHeaderSize + pn, "call size");
        Call call;
        Check(DecodeCall(buf, n, call), "call roundtrip");
        Check(call.sessionId == 9 && call.callId == 3 && call.op == Op::Confirm, "call fields");
        Confirm d;
        Check(DecodeConfirm(call.payload, call.payloadLen, d), "confirm decode");
        Check(strcmp(d.title, "Teleport") == 0 && strcmp(d.message, "Go to Grove Street?") == 0,
              "confirm text");
        Check(!DecodeCall(buf, n - 1, call), "call truncated payload");
        buf[10] = 99;   // op
        Check(!DecodeCall(buf, n, call), "unknown op rejected");
        buf[10] = 2;
        buf[11] = 0xFF; buf[12] = 0xFF;   // payloadLen
        Check(!DecodeCall(buf, n, call), "payload length mismatch rejected");
        Check(EncodeCall(9, 3, Op::Confirm, payload, kMaxCallPayload + 1, buf, sizeof(buf)) == 0,
              "oversized payload not encoded");
        Check(EncodeCall(0, 3, Op::Confirm, payload, pn, buf, sizeof(buf)) == 0, "zero session");
        Confirm bad = c; bad.timeoutSec = 4;
        Check(EncodeConfirm(bad, payload, sizeof(payload)) == 0, "timeout below range");
        bad = c; bad.title[2] = '~';
        Check(EncodeConfirm(bad, payload, sizeof(payload)) == 0, "tilde not allowed");
        bad = c; bad.message[0] = '\n';
        Check(EncodeConfirm(bad, payload, sizeof(payload)) == 0, "control byte not allowed");
        bad = c; bad.titleLen = 0;
        Check(EncodeConfirm(bad, payload, sizeof(payload)) == 0, "empty title not allowed");
        // Decoder must enforce the same rules on hostile input.
        const uint8_t hostile[] = {30, 3, 'a', 0x07, 'b', 1, 'x'};
        Check(!DecodeConfirm(hostile, sizeof(hostile), d), "decoder rejects control byte");
        const uint8_t overlong[] = {30, 65};
        Check(!DecodeConfirm(overlong, sizeof(overlong), d), "decoder rejects title > max");
        const uint8_t trailing[] = {30, 1, 'a', 1, 'b', 0};
        Check(!DecodeConfirm(trailing, sizeof(trailing), d), "decoder rejects trailing byte");
    }

    // ── Notify ──
    {
        Notify nt; nt.kind = NotifyKind::Success; nt.durationSec = 4;
        strcpy(nt.text, "Saved"); nt.textLen = 5;
        const size_t pn = EncodeNotify(nt, payload, sizeof(payload));
        Notify d;
        Check(pn == 8 && DecodeNotify(payload, pn, d) && d.kind == NotifyKind::Success, "notify roundtrip");
        Notify bad = nt; bad.durationSec = 16;
        Check(EncodeNotify(bad, payload, sizeof(payload)) == 0, "notify duration range");
        bad = nt; bad.kind = static_cast<NotifyKind>(4);
        Check(EncodeNotify(bad, payload, sizeof(payload)) == 0, "notify kind range");
        Notify big; big.textLen = static_cast<uint8_t>(kMaxNotifyText);
        memset(big.text, 'x', kMaxNotifyText);
        Check(EncodeNotify(big, payload, sizeof(payload)) == 3 + kMaxNotifyText, "notify max text");
    }

    // ── HudSet / CancelConfirm ──
    {
        HudSet h; h.value = 1;
        const size_t pn = EncodeHudSet(h, payload, sizeof(payload));
        HudSet d;
        Check(pn == 2 && DecodeHudSet(payload, pn, d) && d.value == 1, "hudset roundtrip");
        const uint8_t unknownComponent[] = {2, 1};
        Check(!DecodeHudSet(unknownComponent, 2, d), "unknown hud component rejected");
        const uint8_t badValue[] = {1, 2};
        Check(!DecodeHudSet(badValue, 2, d), "hud value range");
        uint32_t target = 0;
        Check(EncodeCancelConfirm(5, payload, sizeof(payload)) == 4 &&
              DecodeCancelConfirm(payload, 4, target) && target == 5, "cancel roundtrip");
        Check(EncodeCancelConfirm(0, payload, sizeof(payload)) == 0, "cancel zero id");
    }

    // ── Reply + session validation ──
    {
        Reply r; r.sessionId = 9; r.callId = 3; r.op = Op::Confirm; r.result = ReplyResult::Confirmed;
        const size_t n = EncodeReply(r, buf, sizeof(buf));
        Check(n == kReplyHeaderSize, "reply size");
        Reply d;
        Check(DecodeReply(buf, n, d) && d.result == ReplyResult::Confirmed, "reply roundtrip");
        buf[11] = 0;   // result
        Check(!DecodeReply(buf, n, d), "result 0 rejected");
        buf[11] = 7;
        Check(!DecodeReply(buf, n, d), "result 7 rejected");
        Reply big = r; big.payloadLen = kMaxReplyPayload + 1;
        Check(EncodeReply(big, buf, sizeof(buf)) == 0, "reply payload bound");

        PendingConfirm p; p.active = true; p.callId = 3; p.requestId = 42;
        Check(CheckConfirmReply(p, 9, r) == ReplyCheck::Accepted, "matching reply accepted");
        Check(CheckConfirmReply(p, 10, r) == ReplyCheck::StaleSession, "old session rejected");
        Reply other = r; other.callId = 4;
        Check(CheckConfirmReply(p, 9, other) == ReplyCheck::UnknownCall, "unknown call rejected");
        Reply wrongOp = r; wrongOp.op = Op::Notify;
        Check(CheckConfirmReply(p, 9, wrongOp) == ReplyCheck::WrongOp, "wrong op rejected");
        p.active = false;   // resolved by the first reply
        Check(CheckConfirmReply(p, 9, r) == ReplyCheck::UnknownCall, "duplicate reply rejected");
        Check(ToScriptResponse(ReplyResult::Invalid) == ScriptResponse::Error, "invalid -> error");
        Check(ToScriptResponse(ReplyResult::Timeout) == ScriptResponse::Timeout, "timeout mapping");
    }

    // ── Rate limiter ──
    {
        RateLimiter rl;
        int allowed = 0;
        for (int i = 0; i < 25; ++i) allowed += rl.Allow(5000, kMaxCallsPerSecond) ? 1 : 0;
        Check(allowed == static_cast<int>(kMaxCallsPerSecond), "limit within one second");
        Check(rl.Allow(6000, kMaxCallsPerSecond), "next window allows again");
        Check(rl.Allow(1000, kMaxCallsPerSecond), "clock going backwards resets window");
    }

    // ── Reader never reads past the buffer ──
    {
        const uint8_t tiny[1] = {0xE0};
        Call c; Reply r; Hello h; Welcome w;
        Check(!DecodeCall(tiny, 1, c) && !DecodeReply(tiny, 1, r) && !DecodeHello(tiny, 1, h) &&
              !DecodeWelcome(tiny, 1, w), "1-byte input rejected everywhere");
        Check(!DecodeCall(nullptr, 0, c) && !DecodeReply(nullptr, 0, r), "null input rejected");
    }

    std::cout << "native_api: " << checks << " checks passed\n";
    return 0;
}
