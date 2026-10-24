#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "protocol/native_api.h"

// PHONE-M4 — Phone domain protocol codec (revised).
// Client requests (step 9, domain 1) and server responses (Call step 7, ops 16–24).
// Spec: docs/phone/PHONE-M4-PROTOCOL.md (revised 2026-10-24)
//
// All string limits are BYTE counts (not character counts).
// All timestamps are u32 unix seconds (server gettime()).
// All paginated responses use: u32 reqId, u8 count, u8 flags (bit 0 = hasMore).

namespace skr::protocol::phone {

using api::Writer;
using api::Reader;

// ── Phone client request ops (step 9, domain = Phone) ───────────────
enum class ClientOp : uint8_t {
    PhoneSync     = 1,
    ContactList   = 2,
    ContactAdd    = 3,
    ContactRemove = 4,
    SmsThreads    = 5,
    SmsHistory    = 6,
    SmsSend       = 7,
    CallDial      = 8,
    CallAccept    = 9,
    CallDecline   = 10,
    CallHangup    = 11,
    ProfileGet    = 12, // CapPhoneSetup only, empty payload
    MarkRead      = 14, // CapPhoneSetup only, str8 peer + u32 last visible incoming id
    ProfileSave   = 13, // CapPhoneSetup only, phone_profile.h
};
constexpr bool IsKnownClientOp(uint8_t op) { return op >= 1 && op <= 14; }

// ── Phone error codes ───────────────────────────────────────────────
enum class ErrorCode : uint8_t {
    InvalidNumber   = 1,
    Offline         = 2,
    Busy            = 3,
    SelfAction      = 4,
    RateLimited     = 5,
    NoPhone         = 6,
    ContactExists   = 7,
    ContactNotFound = 8,
    ContactsFull    = 9,
    SmsTooLong      = 10,
    InternalError   = 11,
    StaleCall       = 12,
};
constexpr bool IsKnownErrorCode(uint8_t c) { return c >= 1 && c <= 12; }

// ── Call states ─────────────────────────────────────────────────────
enum class CallStatus : uint8_t {
    Idle     = 0,
    Outgoing = 1,
    Ringing  = 2,
    Incoming = 3,
    Active   = 4,
    Ended    = 5,
};

// ── Contact result status ───────────────────────────────────────────
enum class ContactResultStatus : uint8_t {
    Ok    = 0,
    Error = 1,
};

// ── SMS send result status ──────────────────────────────────────────
enum class SmsSendStatus : uint8_t {
    Stored = 0,
    Error  = 1,
};

// ── SMS direction (derived server-side, not stored in DB) ───────────
enum class SmsDirection : uint8_t {
    Outgoing = 0,
    Incoming = 1,
};

// ── Size limits (all byte counts) ───────────────────────────────────
constexpr size_t kPhoneNumberLen    = 7;
constexpr size_t kMaxContactName    = 23;
constexpr size_t kMaxSmsText        = 120;
constexpr size_t kMaxErrorMessage   = 64;
constexpr size_t kMaxPeerName       = 23;
constexpr size_t kMaxPreviewText    = 47;

// ── Page sizes (verified against 400-byte Call payload limit) ───────
constexpr uint8_t kContactListMaxPerPage  = 11;  // 6 + 11*33 = 369 ≤ 400
constexpr uint8_t kSmsThreadMaxPerPage    = 4;   // 6 + 4*86  = 350 ≤ 400
constexpr uint8_t kSmsHistoryMaxPerPage   = 3;   // 6 + 3*131 = 399 ≤ 400

// ── Pagination flags ────────────────────────────────────────────────
constexpr uint8_t kFlagHasMore = 1u << 0;

// ── Phone number validation ─────────────────────────────────────────
inline bool IsValidPhoneNumber(const char* s, size_t n) {
    if (!s || n != kPhoneNumberLen) return false;
    if (s[0] < '1' || s[0] > '9') return false;
    for (size_t i = 1; i < kPhoneNumberLen; ++i)
        if (s[i] < '0' || s[i] > '9') return false;
    return true;
}

// Legacy seven-digit identities remain valid to preserve contacts and SMS.
// Allocation alone is restricted to the 10,000-number M5 pool.
inline bool IsM5PhoneNumber(const char* s, size_t n) {
    return s && IsValidPhoneNumber(s,n) && s[0]=='5' && s[1]=='5' && s[2]=='5';
}
inline bool NormalizePhoneNumber(const char* s, char (&out)[8]) {
    if (!s) return false;
    size_t n=0;
    for (; *s; ++s) {
        if (*s==' ') continue;
        if (n==7 || *s<'0' || *s>'9') return false;
        out[n++]=*s;
    }
    out[n]=0;
    return IsValidPhoneNumber(out,n);
}
inline void FormatPhoneNumber(const char* s, char (&out)[16]) {
    if (!s || !IsValidPhoneNumber(s,std::strlen(s))) { out[0]=0; return; }
    if (IsM5PhoneNumber(s,7)) {
        std::memcpy(out,s,3); out[3]=' '; std::memcpy(out+4,s+3,4); out[8]=0;
    } else { std::memcpy(out,s,8); }
}

// ═══════════════════════════════════════════════════════════════════
// CLIENT REQUEST PAYLOADS (step 9, domain = Phone)
// ═══════════════════════════════════════════════════════════════════

// PhoneSync (op 1): empty payload — no struct needed

// ContactList (op 2): u8 page
struct ContactListRequest {
    uint8_t page = 0;
};
inline size_t EncodeContactListRequest(const ContactListRequest& m, uint8_t* out, size_t cap) {
    Writer w(out, cap);
    w.U8(m.page);
    return w.size();
}
inline bool DecodeContactListRequest(const uint8_t* p, size_t n, ContactListRequest& m) {
    Reader r(p, n);
    m.page = r.U8();
    return r.Done();
}

// ContactAdd (op 3): str8 number(7), str8 displayName(≤23)
struct ContactAddRequest {
    char number[kPhoneNumberLen + 1] = {};
    uint8_t numberLen = 0;
    char name[kMaxContactName + 1] = {};
    uint8_t nameLen = 0;
};
inline size_t EncodeContactAdd(const ContactAddRequest& m, uint8_t* out, size_t cap) {
    if (!IsValidPhoneNumber(m.number, m.numberLen) || !m.nameLen || m.nameLen > kMaxContactName ||
        !api::IsValidText(m.name, m.nameLen)) return 0;
    Writer w(out, cap);
    w.Str8(m.number, m.numberLen);
    w.Str8(m.name, m.nameLen);
    return w.size();
}
inline bool DecodeContactAdd(const uint8_t* p, size_t n, ContactAddRequest& m) {
    Reader r(p, n);
    if (!r.Text8(m.number, kPhoneNumberLen, m.numberLen)) return false;
    if (!r.Text8(m.name, kMaxContactName, m.nameLen)) return false;
    return r.Done() && IsValidPhoneNumber(m.number, m.numberLen);
}

// ContactRemove (op 4): str8 number(7)
struct ContactRemoveRequest {
    char number[kPhoneNumberLen + 1] = {};
    uint8_t numberLen = 0;
};
inline size_t EncodeContactRemove(const ContactRemoveRequest& m, uint8_t* out, size_t cap) {
    if (!IsValidPhoneNumber(m.number, m.numberLen)) return 0;
    Writer w(out, cap);
    w.Str8(m.number, m.numberLen);
    return w.size();
}
inline bool DecodeContactRemove(const uint8_t* p, size_t n, ContactRemoveRequest& m) {
    Reader r(p, n);
    if (!r.Text8(m.number, kPhoneNumberLen, m.numberLen)) return false;
    return r.Done() && IsValidPhoneNumber(m.number, m.numberLen);
}

// SmsThreads (op 5): u8 page
struct SmsThreadsRequest {
    uint8_t page = 0;
};
inline size_t EncodeSmsThreadsRequest(const SmsThreadsRequest& m, uint8_t* out, size_t cap) {
    Writer w(out, cap);
    w.U8(m.page);
    return w.size();
}
inline bool DecodeSmsThreadsRequest(const uint8_t* p, size_t n, SmsThreadsRequest& m) {
    Reader r(p, n);
    m.page = r.U8();
    return r.Done();
}

// SmsHistory (op 6): str8 peerNumber(7), u8 page
struct SmsHistoryRequest {
    char peerNumber[kPhoneNumberLen + 1] = {};
    uint8_t peerNumberLen = 0;
    uint8_t page = 0;
};
inline size_t EncodeSmsHistoryReq(const SmsHistoryRequest& m, uint8_t* out, size_t cap) {
    if (!IsValidPhoneNumber(m.peerNumber, m.peerNumberLen)) return 0;
    Writer w(out, cap);
    w.Str8(m.peerNumber, m.peerNumberLen);
    w.U8(m.page);
    return w.size();
}
inline bool DecodeSmsHistoryReq(const uint8_t* p, size_t n, SmsHistoryRequest& m) {
    Reader r(p, n);
    if (!r.Text8(m.peerNumber, kPhoneNumberLen, m.peerNumberLen)) return false;
    m.page = r.U8();
    return r.Done() && IsValidPhoneNumber(m.peerNumber, m.peerNumberLen);
}

// SmsSend (op 7): str8 recipientNumber(7), str8 text(≤120)
struct SmsSendRequest {
    char recipientNumber[kPhoneNumberLen + 1] = {};
    uint8_t recipientNumberLen = 0;
    char text[kMaxSmsText + 1] = {};
    uint8_t textLen = 0;
};
inline size_t EncodeSmsSend(const SmsSendRequest& m, uint8_t* out, size_t cap) {
    if (!IsValidPhoneNumber(m.recipientNumber, m.recipientNumberLen) ||
        !m.textLen || m.textLen > kMaxSmsText ||
        !api::IsValidText(m.text, m.textLen)) return 0;
    Writer w(out, cap);
    w.Str8(m.recipientNumber, m.recipientNumberLen);
    w.Str8(m.text, m.textLen);
    return w.size();
}
inline bool DecodeSmsSend(const uint8_t* p, size_t n, SmsSendRequest& m) {
    Reader r(p, n);
    if (!r.Text8(m.recipientNumber, kPhoneNumberLen, m.recipientNumberLen)) return false;
    if (!r.Text8(m.text, kMaxSmsText, m.textLen)) return false;
    return r.Done() && IsValidPhoneNumber(m.recipientNumber, m.recipientNumberLen);
}

// CallDial (op 8): str8 number(7)
struct CallDialRequest {
    char number[kPhoneNumberLen + 1] = {};
    uint8_t numberLen = 0;
};
inline size_t EncodeCallDial(const CallDialRequest& m, uint8_t* out, size_t cap) {
    if (!IsValidPhoneNumber(m.number, m.numberLen)) return 0;
    Writer w(out, cap);
    w.Str8(m.number, m.numberLen);
    return w.size();
}
inline bool DecodeCallDial(const uint8_t* p, size_t n, CallDialRequest& m) {
    Reader r(p, n);
    if (!r.Text8(m.number, kPhoneNumberLen, m.numberLen)) return false;
    return r.Done() && IsValidPhoneNumber(m.number, m.numberLen);
}

// CallAccept (op 9): u32 callSessionId
struct CallAcceptRequest {
    uint32_t callSessionId = 0;
};
inline size_t EncodeCallAccept(const CallAcceptRequest& m, uint8_t* out, size_t cap) {
    if (!m.callSessionId) return 0;
    Writer w(out, cap);
    w.U32(m.callSessionId);
    return w.size();
}
inline bool DecodeCallAccept(const uint8_t* p, size_t n, CallAcceptRequest& m) {
    Reader r(p, n);
    m.callSessionId = r.U32();
    return r.Done() && m.callSessionId != 0;
}

// CallDecline (op 10): u32 callSessionId
struct CallDeclineRequest {
    uint32_t callSessionId = 0;
};
inline size_t EncodeCallDecline(const CallDeclineRequest& m, uint8_t* out, size_t cap) {
    if (!m.callSessionId) return 0;
    Writer w(out, cap);
    w.U32(m.callSessionId);
    return w.size();
}
inline bool DecodeCallDecline(const uint8_t* p, size_t n, CallDeclineRequest& m) {
    Reader r(p, n);
    m.callSessionId = r.U32();
    return r.Done() && m.callSessionId != 0;
}

// CallHangup (op 11): u32 callSessionId
struct CallHangupRequest {
    uint32_t callSessionId = 0;
};
inline size_t EncodeCallHangup(const CallHangupRequest& m, uint8_t* out, size_t cap) {
    if (!m.callSessionId) return 0;
    Writer w(out, cap);
    w.U32(m.callSessionId);
    return w.size();
}
inline bool DecodeCallHangup(const uint8_t* p, size_t n, CallHangupRequest& m) {
    Reader r(p, n);
    m.callSessionId = r.U32();
    return r.Done() && m.callSessionId != 0;
}

// ═══════════════════════════════════════════════════════════════════
// SERVER RESPONSE PAYLOADS (Call step 7, ops 16–24)
// ═══════════════════════════════════════════════════════════════════

// PhoneSync (op 16): u32 reqId, str8 ownNumber(7), u8 flags
// Worst case: 4 + (1+7) + 1 = 13 bytes
struct PhoneSyncResponse {
    uint32_t requestId = 0;
    char ownNumber[kPhoneNumberLen + 1] = {};
    uint8_t ownNumberLen = 0;
    uint8_t flags = 0;
};
inline size_t EncodePhoneSync(const PhoneSyncResponse& m, uint8_t* out, size_t cap) {
    if (!IsValidPhoneNumber(m.ownNumber, m.ownNumberLen)) return 0;
    Writer w(out, cap);
    w.U32(m.requestId);
    w.Str8(m.ownNumber, m.ownNumberLen);
    w.U8(m.flags);
    return w.size();
}
inline bool DecodePhoneSync(const uint8_t* p, size_t n, PhoneSyncResponse& m) {
    Reader r(p, n);
    m.requestId = r.U32();
    if (!r.Text8(m.ownNumber, kPhoneNumberLen, m.ownNumberLen)) return false;
    m.flags = r.U8();
    return r.Done() && IsValidPhoneNumber(m.ownNumber, m.ownNumberLen);
}

// ContactList (op 17): u32 reqId, u8 count, u8 flags, [str8 name(≤23), str8 number(7), u8 hue] × count
// Per entry worst case: (1+23) + (1+7) + 1 = 33 bytes
// Worst case: 6 + 11*33 = 369 bytes ≤ 400 ✓
struct ContactEntry {
    char name[kMaxContactName + 1] = {};
    uint8_t nameLen = 0;
    char number[kPhoneNumberLen + 1] = {};
    uint8_t numberLen = 0;
    uint8_t hue = 0;
};
struct ContactListResponse {
    uint32_t requestId = 0;
    uint8_t count = 0;
    uint8_t flags = 0;
    ContactEntry entries[kContactListMaxPerPage];
};
inline size_t EncodeContactList(const ContactListResponse& m, uint8_t* out, size_t cap) {
    if (m.count > kContactListMaxPerPage) return 0;
    Writer w(out, cap);
    w.U32(m.requestId);
    w.U8(m.count);
    w.U8(m.flags);
    for (uint8_t i = 0; i < m.count; ++i) {
        const auto& e = m.entries[i];
        w.Str8(e.name, e.nameLen);
        w.Str8(e.number, e.numberLen);
        w.U8(e.hue);
    }
    return w.size();
}
inline bool DecodeContactList(const uint8_t* p, size_t n, ContactListResponse& m) {
    Reader r(p, n);
    m.requestId = r.U32();
    m.count = r.U8();
    m.flags = r.U8();
    if (!r.ok() || m.count > kContactListMaxPerPage) return false;
    for (uint8_t i = 0; i < m.count; ++i) {
        auto& e = m.entries[i];
        if (!r.Text8(e.name, kMaxContactName, e.nameLen)) return false;
        if (!r.Text8(e.number, kPhoneNumberLen, e.numberLen)) return false;
        e.hue = r.U8();
        if (!r.ok() || !IsValidPhoneNumber(e.number, e.numberLen)) return false;
    }
    return r.Done();
}

// ContactResult (op 18): u32 reqId, u8 status
// 5 bytes
struct ContactResultResponse {
    uint32_t requestId = 0;
    ContactResultStatus status = ContactResultStatus::Ok;
};
inline size_t EncodeContactResult(const ContactResultResponse& m, uint8_t* out, size_t cap) {
    Writer w(out, cap);
    w.U32(m.requestId);
    w.U8(static_cast<uint8_t>(m.status));
    return w.size();
}
inline bool DecodeContactResult(const uint8_t* p, size_t n, ContactResultResponse& m) {
    Reader r(p, n);
    m.requestId = r.U32();
    const uint8_t s = r.U8();
    if (!r.Done() || s > 1) return false;
    m.status = static_cast<ContactResultStatus>(s);
    return true;
}

// SmsThreadList (op 19): u32 reqId, u8 count, u8 flags,
//   [str8 peerName(≤23), str8 peerNumber(7), str8 preview(≤47), u32 ts, u8 unread, u8 hue] × count
// Per entry worst case: (1+23) + (1+7) + (1+47) + 4 + 1 + 1 = 86 bytes
// Worst case: 6 + 4*86 = 350 bytes ≤ 400 ✓
struct ThreadEntry {
    char peerName[kMaxPeerName + 1] = {};
    uint8_t peerNameLen = 0;
    char peerNumber[kPhoneNumberLen + 1] = {};
    uint8_t peerNumberLen = 0;
    char preview[kMaxPreviewText + 1] = {};
    uint8_t previewLen = 0;
    uint32_t timestamp = 0;
    uint8_t unread = 0;
    uint8_t hue = 0;
};
struct SmsThreadListResponse {
    uint32_t requestId = 0;
    uint8_t count = 0;
    uint8_t flags = 0;
    ThreadEntry entries[kSmsThreadMaxPerPage];
};
inline size_t EncodeSmsThreadList(const SmsThreadListResponse& m, uint8_t* out, size_t cap) {
    if (m.count > kSmsThreadMaxPerPage) return 0;
    Writer w(out, cap);
    w.U32(m.requestId);
    w.U8(m.count);
    w.U8(m.flags);
    for (uint8_t i = 0; i < m.count; ++i) {
        const auto& e = m.entries[i];
        w.Str8(e.peerName, e.peerNameLen);
        w.Str8(e.peerNumber, e.peerNumberLen);
        w.Str8(e.preview, e.previewLen);
        w.U32(e.timestamp);
        w.U8(e.unread);
        w.U8(e.hue);
    }
    return w.size();
}
inline bool DecodeSmsThreadList(const uint8_t* p, size_t n, SmsThreadListResponse& m) {
    Reader r(p, n);
    m.requestId = r.U32();
    m.count = r.U8();
    m.flags = r.U8();
    if (!r.ok() || m.count > kSmsThreadMaxPerPage) return false;
    for (uint8_t i = 0; i < m.count; ++i) {
        auto& e = m.entries[i];
        if (!r.Text8(e.peerName, kMaxPeerName, e.peerNameLen)) return false;
        if (!r.Text8(e.peerNumber, kPhoneNumberLen, e.peerNumberLen)) return false;
        if (!r.Text8(e.preview, kMaxPreviewText, e.previewLen)) return false;
        e.timestamp = r.U32();
        e.unread = r.U8();
        e.hue = r.U8();
        if (!r.ok() || !IsValidPhoneNumber(e.peerNumber, e.peerNumberLen)) return false;
    }
    return r.Done();
}

// SmsHistory (op 20): u32 reqId, u8 count, u8 flags,
//   [u32 msgId, str8 text(≤120), u32 ts, u8 delivery, u8 direction] × count
// Per entry worst case: 4 + (1+120) + 4 + 1 + 1 = 131 bytes
// Worst case: 6 + 3*131 = 399 bytes ≤ 400 ✓
struct MessageEntry {
    uint32_t messageId = 0;
    char text[kMaxSmsText + 1] = {};
    uint8_t textLen = 0;
    uint32_t timestamp = 0;
    uint8_t delivery = 0;
    SmsDirection direction = SmsDirection::Outgoing;
};
struct SmsHistoryResponse {
    uint32_t requestId = 0;
    uint8_t count = 0;
    uint8_t flags = 0;
    MessageEntry entries[kSmsHistoryMaxPerPage];
};
inline size_t EncodeSmsHistory(const SmsHistoryResponse& m, uint8_t* out, size_t cap) {
    if (m.count > kSmsHistoryMaxPerPage) return 0;
    Writer w(out, cap);
    w.U32(m.requestId);
    w.U8(m.count);
    w.U8(m.flags);
    for (uint8_t i = 0; i < m.count; ++i) {
        const auto& e = m.entries[i];
        w.U32(e.messageId);
        w.Str8(e.text, e.textLen);
        w.U32(e.timestamp);
        w.U8(e.delivery);
        w.U8(static_cast<uint8_t>(e.direction));
    }
    return w.size();
}
inline bool DecodeSmsHistory(const uint8_t* p, size_t n, SmsHistoryResponse& m) {
    Reader r(p, n);
    m.requestId = r.U32();
    m.count = r.U8();
    m.flags = r.U8();
    if (!r.ok() || m.count > kSmsHistoryMaxPerPage) return false;
    for (uint8_t i = 0; i < m.count; ++i) {
        auto& e = m.entries[i];
        e.messageId = r.U32();
        if (!r.Text8(e.text, kMaxSmsText, e.textLen)) return false;
        e.timestamp = r.U32();
        e.delivery = r.U8();
        const uint8_t dir = r.U8();
        if (!r.ok() || dir > 1) return false;
        e.direction = static_cast<SmsDirection>(dir);
    }
    return r.Done();
}

// SmsSendResult (op 21): u32 reqId, u8 status, u32 messageId
// 9 bytes
struct SmsSendResultResponse {
    uint32_t requestId = 0;
    SmsSendStatus status = SmsSendStatus::Stored;
    uint32_t messageId = 0;
};
inline size_t EncodeSmsSendResult(const SmsSendResultResponse& m, uint8_t* out, size_t cap) {
    Writer w(out, cap);
    w.U32(m.requestId);
    w.U8(static_cast<uint8_t>(m.status));
    w.U32(m.messageId);
    return w.size();
}
inline bool DecodeSmsSendResult(const uint8_t* p, size_t n, SmsSendResultResponse& m) {
    Reader r(p, n);
    m.requestId = r.U32();
    const uint8_t s = r.U8();
    m.messageId = r.U32();
    if (!r.Done() || s > 1) return false;
    m.status = static_cast<SmsSendStatus>(s);
    return true;
}

// SmsReceived (op 22): str8 senderName(≤23), str8 senderNumber(7), str8 text(≤120),
//   u32 ts, u32 messageId, u8 hue
// Worst case: (1+23) + (1+7) + (1+120) + 4 + 4 + 1 = 162 bytes
struct SmsReceivedPush {
    char senderName[kMaxPeerName + 1] = {};
    uint8_t senderNameLen = 0;
    char senderNumber[kPhoneNumberLen + 1] = {};
    uint8_t senderNumberLen = 0;
    char text[kMaxSmsText + 1] = {};
    uint8_t textLen = 0;
    uint32_t timestamp = 0;
    uint32_t messageId = 0;
    uint8_t hue = 0;
};
inline size_t EncodeSmsReceived(const SmsReceivedPush& m, uint8_t* out, size_t cap) {
    if (!m.senderNameLen || !m.textLen ||
        !IsValidPhoneNumber(m.senderNumber, m.senderNumberLen)) return 0;
    Writer w(out, cap);
    w.Str8(m.senderName, m.senderNameLen);
    w.Str8(m.senderNumber, m.senderNumberLen);
    w.Str8(m.text, m.textLen);
    w.U32(m.timestamp);
    w.U32(m.messageId);
    w.U8(m.hue);
    return w.size();
}
inline bool DecodeSmsReceived(const uint8_t* p, size_t n, SmsReceivedPush& m) {
    Reader r(p, n);
    if (!r.Text8(m.senderName, kMaxPeerName, m.senderNameLen)) return false;
    if (!r.Text8(m.senderNumber, kPhoneNumberLen, m.senderNumberLen)) return false;
    if (!r.Text8(m.text, kMaxSmsText, m.textLen)) return false;
    m.timestamp = r.U32();
    m.messageId = r.U32();
    m.hue = r.U8();
    return r.Done() && IsValidPhoneNumber(m.senderNumber, m.senderNumberLen);
}

// CallState (op 23): u32 reqId, u32 callSessionId, u8 state,
//   str8 peerName(≤23), str8 peerNumber(7), u8 peerHue, u32 startTime
// Worst case: 4 + 4 + 1 + (1+23) + (1+7) + 1 + 4 = 46 bytes
struct CallStateResponse {
    uint32_t requestId = 0;
    uint32_t callSessionId = 0;
    CallStatus state = CallStatus::Idle;
    char peerName[kMaxPeerName + 1] = {};
    uint8_t peerNameLen = 0;
    char peerNumber[kPhoneNumberLen + 1] = {};
    uint8_t peerNumberLen = 0;
    uint8_t peerHue = 0;
    uint32_t startTime = 0;
};
inline size_t EncodeCallState(const CallStateResponse& m, uint8_t* out, size_t cap) {
    Writer w(out, cap);
    w.U32(m.requestId);
    w.U32(m.callSessionId);
    w.U8(static_cast<uint8_t>(m.state));
    w.Str8(m.peerName, m.peerNameLen);
    w.Str8(m.peerNumber, m.peerNumberLen);
    w.U8(m.peerHue);
    w.U32(m.startTime);
    return w.size();
}
inline bool DecodeCallState(const uint8_t* p, size_t n, CallStateResponse& m) {
    Reader r(p, n);
    m.requestId = r.U32();
    m.callSessionId = r.U32();
    const uint8_t s = r.U8();
    if (!r.ok() || s > 5) return false;
    m.state = static_cast<CallStatus>(s);
    if (!r.Text8(m.peerName, kMaxPeerName, m.peerNameLen)) return false;
    if (!r.Text8(m.peerNumber, kPhoneNumberLen, m.peerNumberLen)) return false;
    m.peerHue = r.U8();
    m.startTime = r.U32();
    return r.Done();
}

// PhoneError (op 24): u32 reqId, u8 errorCode, str8 message(≤64)
// Worst case: 4 + 1 + (1+64) = 70 bytes
struct PhoneErrorResponse {
    uint32_t requestId = 0;
    ErrorCode code = ErrorCode::InternalError;
    char message[kMaxErrorMessage + 1] = {};
    uint8_t messageLen = 0;
};
inline size_t EncodePhoneError(const PhoneErrorResponse& m, uint8_t* out, size_t cap) {
    if (!IsKnownErrorCode(static_cast<uint8_t>(m.code))) return 0;
    Writer w(out, cap);
    w.U32(m.requestId);
    w.U8(static_cast<uint8_t>(m.code));
    w.Str8(m.message, m.messageLen);
    return w.size();
}
inline bool DecodePhoneError(const uint8_t* p, size_t n, PhoneErrorResponse& m) {
    Reader r(p, n);
    m.requestId = r.U32();
    const uint8_t c = r.U8();
    if (!r.ok() || !IsKnownErrorCode(c)) return false;
    m.code = static_cast<ErrorCode>(c);
    if (!r.Text8(m.message, kMaxErrorMessage, m.messageLen)) return false;
    return r.Done();
}

} // namespace skr::protocol::phone
