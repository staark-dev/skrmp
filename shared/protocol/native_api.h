#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "protocol/enhanced.h"

// SKR Native Runtime API v1 (TASK API-001). Full spec: docs/protocol/SKR_NATIVE_API.md
//
// Transport: the existing SKR Enhanced envelope (enhanced.h), no new RPC IDs.
//   client -> server: RPC 240 (client_state.h), first byte kEnhancedMarker
//   server -> client: RPC 241 (session_info.h), first byte kEnhancedMarker
// Envelope steps 1..3 = Enhanced handshake, 4 = rejection (rejection.h).
// This API owns steps 5..8 and is only used after the Enhanced handshake is
// confirmed on both sides. RPC 242/243 are not involved.
//
// Wire rules: byte-aligned, little-endian, every message length-exact
// (trailing bytes are an error), strings are length-prefixed and never
// NUL-terminated on the wire. Unknown steps/ops/enums are rejected.
// The client never executes anything it receives: every op is a fixed,
// typed UI action with validated arguments.

namespace skr::protocol::api {

enum class Step : uint8_t {
    Hello         = 5,  // C->S  offer API versions + client capabilities
    Welcome       = 6,  // S->C  chosen version, session id, negotiated capabilities
    Call          = 7,  // S->C  typed operation
    Reply         = 8,  // C->S  result of a call that expects one
    ClientRequest = 9,  // C->S  client-initiated domain request (PHONE-M4)
};
constexpr bool IsApiStep(uint8_t step) { return step >= 5 && step <= 9; }

constexpr uint8_t kVersionMin = 1;
constexpr uint8_t kVersionMax = 1;

enum Capability : uint32_t {
    CapNotify   = 1u << 0,
    CapConfirm  = 1u << 1,
    CapHudValue = 1u << 2,
    CapPhone    = 1u << 3,
    CapGateway  = 1u << 4,
    CapPhoneGateway = 1u << 6, // explicit PHONE namespace v1 support
    CapPhoneSetup = 1u << 5, // M5 persistent activation/profile
};
constexpr uint32_t kKnownCapabilities = CapNotify | CapConfirm | CapHudValue | CapPhone | CapGateway | CapPhoneSetup | CapPhoneGateway;

enum class WelcomeStatus : uint8_t { Accepted = 0, VersionUnsupported = 1 };

enum class Op : uint8_t {
    // Core (1..15)
    Notify        = 1,  // fire-and-forget toast
    Confirm       = 2,  // modal yes/no, always answered by exactly one Reply
    HudSet        = 3,  // one supported HUD value (session-only override)
    CancelConfirm = 4,  // server withdraws a pending Confirm (no Reply)
    // Phone domain S->C (16..24)
    PhoneSync       = 16,
    ContactList     = 17,
    ContactResult   = 18,
    SmsThreadList   = 19,
    SmsHistory      = 20,
    SmsSendResult   = 21,
    SmsReceived     = 22,
    CallState       = 23,
    PhoneError      = 24,
    PhoneReadResult = 26, // CapPhoneSetup, u32 acknowledged request id
    PhoneProfile    = 25, // requires CapPhoneSetup
};
constexpr bool IsCoreOp(uint8_t op) { return op >= 1 && op <= 4; }
constexpr bool IsPhoneOp(uint8_t op) { return op >= 16 && op <= 26; }
constexpr bool IsKnownOp(uint8_t op) { return IsCoreOp(op) || IsPhoneOp(op); }
constexpr uint32_t CapabilityFor(Op op) {
    if (op == Op::PhoneProfile || op == Op::PhoneReadResult) return CapPhoneSetup;
    if (IsPhoneOp(static_cast<uint8_t>(op))) return CapPhone;
    return op == Op::Notify ? CapNotify
         : op == Op::HudSet ? CapHudValue
         : CapConfirm;   // Confirm, CancelConfirm
}

enum class ReplyResult : uint8_t {
    Confirmed   = 1,
    Cancelled   = 2,  // player declined
    Busy        = 3,  // another dialog is visible
    Unsupported = 4,  // op/capability not available on this client
    Timeout     = 5,  // client-side countdown ran out
    Invalid     = 6,  // payload failed client validation
};
constexpr bool IsKnownResult(uint8_t r) { return r >= 1 && r <= 6; }

enum class NotifyKind : uint8_t { Info = 0, Success = 1, Warning = 2, Error = 3 };
enum class HudComponent : uint8_t { VitalsNumbers = 1 };

// ── Size limits ──────────────────────────────────────────────────────
constexpr size_t kHelloSize        = 12;
constexpr size_t kWelcomeSize      = 16;
constexpr size_t kCallHeaderSize   = 13;
constexpr size_t kReplyHeaderSize  = 14;
constexpr size_t kMaxCallPayload   = 400;
constexpr size_t kMaxReplyPayload  = 32;
constexpr size_t kClientRequestHeaderSize = 14;
constexpr size_t kMaxClientRequestPayload = 200;
constexpr size_t kMaxCallSize      = kCallHeaderSize + kMaxCallPayload;
constexpr size_t kMaxReplySize     = kReplyHeaderSize + kMaxReplyPayload;
constexpr size_t kMaxClientRequestSize = kClientRequestHeaderSize + kMaxClientRequestPayload;
constexpr size_t kMaxInboundSize   = kMaxClientRequestSize;  // largest C->S message

constexpr size_t  kMaxNotifyText     = 144;
constexpr uint8_t kNotifyMinSec      = 1;
constexpr uint8_t kNotifyMaxSec      = 15;
constexpr size_t  kMaxConfirmTitle   = 64;
constexpr size_t  kMaxConfirmMessage = 200;
constexpr uint8_t kConfirmMinSec     = 5;
constexpr uint8_t kConfirmMaxSec     = 120;

// ── Timing / abuse limits ────────────────────────────────────────────
constexpr uint64_t kWelcomeTimeoutMs    = 5000;  // client gives up (old/plain server)
constexpr uint64_t kConfirmGraceMs      = 3000;  // server waits this long past the client countdown
constexpr uint32_t kMaxCallsPerSecond   = 10;    // server->client, per player (script spam guard)
constexpr uint32_t kMaxRepliesPerSecond = 10;    // client->server, per player
constexpr uint32_t kMaxInvalidMessages  = 20;    // per session; further API traffic is ignored

// ── Text ─────────────────────────────────────────────────────────────
// Printable single-byte text (SA-MP ANSI). Control bytes, DEL and '~'
// (CFont token prefix) are not allowed on the wire.
constexpr bool IsAllowedTextByte(uint8_t c) { return c >= 0x20 && c != 0x7F && c != '~'; }
inline bool IsValidText(const char* s, size_t n) {
    if (!s || n == 0) return false;
    for (size_t i = 0; i < n; ++i)
        if (!IsAllowedTextByte(static_cast<uint8_t>(s[i]))) return false;
    return true;
}
// In-place: replaces disallowed bytes with ' '. Used on script-supplied text.
inline void SanitizeText(char* s, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (!IsAllowedTextByte(static_cast<uint8_t>(s[i]))) s[i] = ' ';
}

// ── Byte writer / reader ─────────────────────────────────────────────
class Writer {
public:
    Writer(uint8_t* buf, size_t cap) : buf_(buf), cap_(cap) {}
    void U8(uint8_t v) { Put(&v, 1); }
    void U16(uint16_t v) { uint8_t b[2] = {uint8_t(v), uint8_t(v >> 8)}; Put(b, 2); }
    void U32(uint32_t v) {
        uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
        Put(b, 4);
    }
    void Bytes(const void* p, size_t n) { Put(p, n); }
    // u8 length + bytes
    void Str8(const char* s, size_t n) {
        if (n > 255) { ok_ = false; return; }
        U8(static_cast<uint8_t>(n));
        Put(s, n);
    }
    bool ok() const { return ok_; }
    size_t size() const { return ok_ ? len_ : 0; }
private:
    void Put(const void* p, size_t n) {
        if (!ok_ || n > cap_ - len_) { ok_ = false; return; }
        if (n) memcpy(buf_ + len_, p, n);
        len_ += n;
    }
    uint8_t* buf_;
    size_t cap_;
    size_t len_ = 0;
    bool ok_ = true;
};

class Reader {
public:
    Reader(const uint8_t* p, size_t n) : p_(p), n_(n) {}
    uint8_t U8() { uint8_t v = 0; if (Need(1)) v = p_[pos_++]; return v; }
    uint16_t U16() {
        uint16_t v = 0;
        if (Need(2)) { v = uint16_t(p_[pos_] | (p_[pos_ + 1] << 8)); pos_ += 2; }
        return v;
    }
    uint32_t U32() {
        uint32_t v = 0;
        if (Need(4)) {
            v = uint32_t(p_[pos_]) | (uint32_t(p_[pos_ + 1]) << 8) |
                (uint32_t(p_[pos_ + 2]) << 16) | (uint32_t(p_[pos_ + 3]) << 24);
            pos_ += 4;
        }
        return v;
    }
    const uint8_t* Bytes(size_t n) {
        if (!Need(n)) return nullptr;
        const uint8_t* r = p_ + pos_;
        pos_ += n;
        return r;
    }
    // u8 length + text in [1, maxLen], copied NUL-terminated into out[maxLen + 1].
    bool Text8(char* out, size_t maxLen, uint8_t& lenOut) {
        const uint8_t n = U8();
        if (!ok_ || n == 0 || n > maxLen) { ok_ = false; return false; }
        const uint8_t* s = Bytes(n);
        if (!s) return false;
        memcpy(out, s, n);
        out[n] = '\0';
        lenOut = n;
        if (!IsValidText(out, n)) { ok_ = false; return false; }
        return true;
    }
    bool ok() const { return ok_; }
    bool Done() const { return ok_ && pos_ == n_; }   // exact length consumed
private:
    bool Need(size_t n) {
        if (!ok_ || n > n_ - pos_) { ok_ = false; return false; }
        return true;
    }
    const uint8_t* p_;
    size_t n_;
    size_t pos_ = 0;
    bool ok_ = true;
};

inline bool HasEnvelope(const uint8_t* p, size_t n, Step step) {
    return p && n >= 2 && p[0] == kEnhancedMarker && p[1] == static_cast<uint8_t>(step);
}

// Highest version both sides support, 0 when there is none.
constexpr uint8_t ChooseVersion(uint8_t peerMin, uint8_t peerMax) {
    const uint8_t lo = peerMin > kVersionMin ? peerMin : kVersionMin;
    const uint8_t hi = peerMax < kVersionMax ? peerMax : kVersionMax;
    return (peerMin == 0 || peerMin > peerMax || lo > hi) ? 0 : hi;
}

// ── Hello (C->S, 12 bytes) ───────────────────────────────────────────
// u8 marker, u8 step=5, u8 versionMin, u8 versionMax,
// u32 enhancedToken (Offer token of this connection), u32 capabilities
struct Hello {
    uint8_t versionMin = kVersionMin;
    uint8_t versionMax = kVersionMax;
    uint32_t enhancedToken = 0;
    uint32_t capabilities = 0;
};
inline size_t EncodeHello(const Hello& m, uint8_t* out, size_t cap) {
    Writer w(out, cap);
    w.U8(kEnhancedMarker); w.U8(static_cast<uint8_t>(Step::Hello));
    w.U8(m.versionMin); w.U8(m.versionMax);
    w.U32(m.enhancedToken); w.U32(m.capabilities);
    return w.size();
}
// Unknown capability bits are tolerated (masked by the caller), malformed
// version ranges and a zero token are not.
inline bool DecodeHello(const uint8_t* p, size_t n, Hello& m) {
    if (n != kHelloSize || !HasEnvelope(p, n, Step::Hello)) return false;
    Reader r(p + 2, n - 2);
    m.versionMin = r.U8(); m.versionMax = r.U8();
    m.enhancedToken = r.U32(); m.capabilities = r.U32();
    return r.Done() && m.versionMin != 0 && m.versionMin <= m.versionMax && m.enhancedToken != 0;
}

// ── Welcome (S->C, 16 bytes) ─────────────────────────────────────────
// u8 marker, u8 step=6, u8 version, u8 status, u32 sessionId,
// u32 capabilities (negotiated), u16 maxCallPayload, u16 reserved = 0
struct Welcome {
    uint8_t version = 0;
    WelcomeStatus status = WelcomeStatus::Accepted;
    uint32_t sessionId = 0;
    uint32_t capabilities = 0;
    uint16_t maxCallPayload = static_cast<uint16_t>(kMaxCallPayload);
};
inline size_t EncodeWelcome(const Welcome& m, uint8_t* out, size_t cap) {
    Writer w(out, cap);
    w.U8(kEnhancedMarker); w.U8(static_cast<uint8_t>(Step::Welcome));
    w.U8(m.version); w.U8(static_cast<uint8_t>(m.status));
    w.U32(m.sessionId); w.U32(m.capabilities);
    w.U16(m.maxCallPayload); w.U16(0);
    return w.size();
}
inline bool DecodeWelcome(const uint8_t* p, size_t n, Welcome& m) {
    if (n != kWelcomeSize || !HasEnvelope(p, n, Step::Welcome)) return false;
    Reader r(p + 2, n - 2);
    m.version = r.U8();
    const uint8_t status = r.U8();
    m.sessionId = r.U32(); m.capabilities = r.U32();
    m.maxCallPayload = r.U16();
    const uint16_t reserved = r.U16();
    if (!r.Done() || reserved != 0 || status > 1) return false;
    m.status = static_cast<WelcomeStatus>(status);
    if (m.status != WelcomeStatus::Accepted) return true;
    return m.version >= kVersionMin && m.version <= kVersionMax && m.sessionId != 0 &&
           (m.capabilities & ~kKnownCapabilities) == 0 && m.maxCallPayload <= kMaxCallPayload;
}

// ── Call (S->C, 13 + payload) ────────────────────────────────────────
// u8 marker, u8 step=7, u32 sessionId, u32 callId, u8 op, u16 payloadLen, payload
struct Call {
    uint32_t sessionId = 0;
    uint32_t callId = 0;
    Op op = Op::Notify;
    const uint8_t* payload = nullptr;   // points into the decoded buffer
    uint16_t payloadLen = 0;
};
inline size_t EncodeCall(uint32_t sessionId, uint32_t callId, Op op,
                         const uint8_t* payload, size_t payloadLen, uint8_t* out, size_t cap) {
    if (payloadLen > kMaxCallPayload || !sessionId || !callId) return 0;
    Writer w(out, cap);
    w.U8(kEnhancedMarker); w.U8(static_cast<uint8_t>(Step::Call));
    w.U32(sessionId); w.U32(callId); w.U8(static_cast<uint8_t>(op));
    w.U16(static_cast<uint16_t>(payloadLen)); w.Bytes(payload, payloadLen);
    return w.size();
}
inline bool DecodeCall(const uint8_t* p, size_t n, Call& m) {
    if (n < kCallHeaderSize || n > kMaxCallSize || !HasEnvelope(p, n, Step::Call)) return false;
    Reader r(p + 2, n - 2);
    m.sessionId = r.U32(); m.callId = r.U32();
    const uint8_t op = r.U8();
    m.payloadLen = r.U16();
    if (!r.ok() || m.payloadLen != n - kCallHeaderSize) return false;
    m.payload = r.Bytes(m.payloadLen);
    if (!r.Done() || !IsKnownOp(op) || !m.sessionId || !m.callId) return false;
    m.op = static_cast<Op>(op);
    return true;
}

// ── Reply (C->S, 14 + payload) ───────────────────────────────────────
// u8 marker, u8 step=8, u32 sessionId, u32 callId, u8 op, u8 result,
// u16 payloadLen (<= 32, v1 always 0), payload
struct Reply {
    uint32_t sessionId = 0;
    uint32_t callId = 0;
    Op op = Op::Confirm;
    ReplyResult result = ReplyResult::Cancelled;
    const uint8_t* payload = nullptr;
    uint16_t payloadLen = 0;
};
inline size_t EncodeReply(const Reply& m, uint8_t* out, size_t cap) {
    if (m.payloadLen > kMaxReplyPayload || !m.sessionId || !m.callId) return 0;
    Writer w(out, cap);
    w.U8(kEnhancedMarker); w.U8(static_cast<uint8_t>(Step::Reply));
    w.U32(m.sessionId); w.U32(m.callId);
    w.U8(static_cast<uint8_t>(m.op)); w.U8(static_cast<uint8_t>(m.result));
    w.U16(m.payloadLen); w.Bytes(m.payload, m.payloadLen);
    return w.size();
}
inline bool DecodeReply(const uint8_t* p, size_t n, Reply& m) {
    if (n < kReplyHeaderSize || n > kMaxReplySize || !HasEnvelope(p, n, Step::Reply)) return false;
    Reader r(p + 2, n - 2);
    m.sessionId = r.U32(); m.callId = r.U32();
    const uint8_t op = r.U8(), result = r.U8();
    m.payloadLen = r.U16();
    if (!r.ok() || m.payloadLen != n - kReplyHeaderSize) return false;
    m.payload = r.Bytes(m.payloadLen);
    if (!r.Done() || !IsKnownOp(op) || !IsKnownResult(result) || !m.sessionId || !m.callId)
        return false;
    m.op = static_cast<Op>(op);
    m.result = static_cast<ReplyResult>(result);
    return true;
}

// ── ClientRequest (C->S, 14 + payload) ──────────────────────────────
// u8 marker, u8 step=9, u32 sessionId, u32 requestId, u8 domain, u8 op,
// u16 payloadLen, payload
enum class RequestDomain : uint8_t { Phone = 1 };
constexpr bool IsKnownDomain(uint8_t d) { return d == 1; }

struct ClientRequest {
    uint32_t sessionId = 0;
    uint32_t requestId = 0;
    RequestDomain domain = RequestDomain::Phone;
    uint8_t op = 0;
    const uint8_t* payload = nullptr;
    uint16_t payloadLen = 0;
};
inline size_t EncodeClientRequest(const ClientRequest& m, uint8_t* out, size_t cap) {
    if (m.payloadLen > kMaxClientRequestPayload || !m.sessionId || !m.requestId) return 0;
    Writer w(out, cap);
    w.U8(kEnhancedMarker); w.U8(static_cast<uint8_t>(Step::ClientRequest));
    w.U32(m.sessionId); w.U32(m.requestId);
    w.U8(static_cast<uint8_t>(m.domain)); w.U8(m.op);
    w.U16(m.payloadLen); w.Bytes(m.payload, m.payloadLen);
    return w.size();
}
inline bool DecodeClientRequest(const uint8_t* p, size_t n, ClientRequest& m) {
    if (n < kClientRequestHeaderSize || n > kMaxClientRequestSize ||
        !HasEnvelope(p, n, Step::ClientRequest)) return false;
    Reader r(p + 2, n - 2);
    m.sessionId = r.U32(); m.requestId = r.U32();
    const uint8_t domain = r.U8();
    m.op = r.U8();
    m.payloadLen = r.U16();
    if (!r.ok() || m.payloadLen != n - kClientRequestHeaderSize) return false;
    m.payload = r.Bytes(m.payloadLen);
    if (!r.Done() || !IsKnownDomain(domain) || !m.sessionId || !m.requestId)
        return false;
    m.domain = static_cast<RequestDomain>(domain);
    return true;
}

// ── Call payloads ────────────────────────────────────────────────────
// Notify:  u8 kind, u8 durationSec, u8 textLen, text
struct Notify {
    NotifyKind kind = NotifyKind::Info;
    uint8_t durationSec = 5;
    char text[kMaxNotifyText + 1] = {};
    uint8_t textLen = 0;
};
inline size_t EncodeNotify(const Notify& m, uint8_t* out, size_t cap) {
    if (static_cast<uint8_t>(m.kind) > 3 || m.durationSec < kNotifyMinSec ||
        m.durationSec > kNotifyMaxSec || m.textLen > kMaxNotifyText || !IsValidText(m.text, m.textLen))
        return 0;
    Writer w(out, cap);
    w.U8(static_cast<uint8_t>(m.kind)); w.U8(m.durationSec); w.Str8(m.text, m.textLen);
    return w.size();
}
inline bool DecodeNotify(const uint8_t* p, size_t n, Notify& m) {
    Reader r(p, n);
    const uint8_t kind = r.U8();
    m.durationSec = r.U8();
    if (!r.Text8(m.text, kMaxNotifyText, m.textLen) || !r.Done()) return false;
    if (kind > 3 || m.durationSec < kNotifyMinSec || m.durationSec > kNotifyMaxSec) return false;
    m.kind = static_cast<NotifyKind>(kind);
    return true;
}

// Confirm: u8 timeoutSec, u8 titleLen, title, u8 messageLen, message
struct Confirm {
    uint8_t timeoutSec = 30;
    char title[kMaxConfirmTitle + 1] = {};
    uint8_t titleLen = 0;
    char message[kMaxConfirmMessage + 1] = {};
    uint8_t messageLen = 0;
};
inline size_t EncodeConfirm(const Confirm& m, uint8_t* out, size_t cap) {
    if (m.timeoutSec < kConfirmMinSec || m.timeoutSec > kConfirmMaxSec ||
        m.titleLen > kMaxConfirmTitle || m.messageLen > kMaxConfirmMessage ||
        !IsValidText(m.title, m.titleLen) || !IsValidText(m.message, m.messageLen))
        return 0;
    Writer w(out, cap);
    w.U8(m.timeoutSec); w.Str8(m.title, m.titleLen); w.Str8(m.message, m.messageLen);
    return w.size();
}
inline bool DecodeConfirm(const uint8_t* p, size_t n, Confirm& m) {
    Reader r(p, n);
    m.timeoutSec = r.U8();
    if (!r.Text8(m.title, kMaxConfirmTitle, m.titleLen)) return false;
    if (!r.Text8(m.message, kMaxConfirmMessage, m.messageLen) || !r.Done()) return false;
    return m.timeoutSec >= kConfirmMinSec && m.timeoutSec <= kConfirmMaxSec;
}

// HudSet: u8 component, u8 value (0/1)
struct HudSet { HudComponent component = HudComponent::VitalsNumbers; uint8_t value = 0; };
inline size_t EncodeHudSet(const HudSet& m, uint8_t* out, size_t cap) {
    if (m.component != HudComponent::VitalsNumbers || m.value > 1) return 0;
    Writer w(out, cap);
    w.U8(static_cast<uint8_t>(m.component)); w.U8(m.value);
    return w.size();
}
inline bool DecodeHudSet(const uint8_t* p, size_t n, HudSet& m) {
    Reader r(p, n);
    const uint8_t component = r.U8();
    m.value = r.U8();
    if (!r.Done() || component != static_cast<uint8_t>(HudComponent::VitalsNumbers) || m.value > 1)
        return false;
    m.component = static_cast<HudComponent>(component);
    return true;
}

// CancelConfirm: u32 targetCallId
inline size_t EncodeCancelConfirm(uint32_t targetCallId, uint8_t* out, size_t cap) {
    if (!targetCallId) return 0;
    Writer w(out, cap);
    w.U32(targetCallId);
    return w.size();
}
inline bool DecodeCancelConfirm(const uint8_t* p, size_t n, uint32_t& targetCallId) {
    Reader r(p, n);
    targetCallId = r.U32();
    return r.Done() && targetCallId != 0;
}

// ── Server-side helpers (pure, unit-tested) ──────────────────────────
// Fixed one-second window. Allow() counts the event when it returns true.
struct RateLimiter {
    uint64_t windowStartMs = 0;
    uint32_t count = 0;
    bool Allow(uint64_t nowMs, uint32_t maxPerSecond) {
        if (nowMs - windowStartMs >= 1000 || nowMs < windowStartMs) { windowStartMs = nowMs; count = 0; }
        if (count >= maxPerSecond) return false;
        ++count;
        return true;
    }
};

// At most one Confirm outstanding per player and session.
struct PendingConfirm {
    bool active = false;
    uint32_t callId = 0;
    int32_t requestId = 0;      // script-chosen id, echoed to OnSKREConfirmResponse
    uint64_t deadlineMs = 0;
};

enum class ReplyCheck : uint8_t {
    Accepted,      // matches the pending Confirm: resolve it
    StaleSession,  // session id from an earlier connection/session
    UnknownCall,   // no pending call with this id (duplicate, late, or forged)
    WrongOp,       // reply op does not match the pending call
};
inline ReplyCheck CheckConfirmReply(const PendingConfirm& pending, uint32_t sessionId, const Reply& r) {
    if (r.sessionId != sessionId) return ReplyCheck::StaleSession;
    if (!pending.active || r.callId != pending.callId) return ReplyCheck::UnknownCall;
    if (r.op != Op::Confirm) return ReplyCheck::WrongOp;
    return ReplyCheck::Accepted;   // every known result code is a valid Confirm answer
}
constexpr const char* ReplyCheckName(ReplyCheck c) {
    return c == ReplyCheck::Accepted ? "accepted" : c == ReplyCheck::StaleSession ? "stale-session"
         : c == ReplyCheck::UnknownCall ? "unknown-call" : "wrong-op";
}

// Script-facing response codes (skr_enhanced.inc SKRE_RESPONSE).
enum class ScriptResponse : int32_t {
    Confirmed   = 1,
    Cancelled   = 2,
    Busy        = 3,
    Timeout     = 4,  // client countdown or server deadline
    Unsupported = 5,
    Error       = 6,  // client rejected the request as invalid
};
constexpr ScriptResponse ToScriptResponse(ReplyResult r) {
    return r == ReplyResult::Confirmed ? ScriptResponse::Confirmed
         : r == ReplyResult::Cancelled ? ScriptResponse::Cancelled
         : r == ReplyResult::Busy      ? ScriptResponse::Busy
         : r == ReplyResult::Timeout   ? ScriptResponse::Timeout
         : r == ReplyResult::Unsupported ? ScriptResponse::Unsupported
         : ScriptResponse::Error;
}

} // namespace skr::protocol::api
