// Unit tests for shared/protocol/phone_protocol.h (PHONE-M4).
#include "protocol/phone_protocol.h"
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

void SetStr(char* dst, uint8_t& lenOut, const char* src) {
    lenOut = static_cast<uint8_t>(strlen(src));
    memcpy(dst, src, lenOut);
    dst[lenOut] = '\0';
}
} // namespace

int main() {
    using namespace skr::protocol;
    using namespace skr::protocol::api;
    using namespace skr::protocol::phone;

    uint8_t buf[512];

    // ── Phone number validation ─────────────────────────────────────
    Check(IsValidPhoneNumber("1000000", 7), "valid number min");
    Check(IsValidPhoneNumber("9999999", 7), "valid number max");
    Check(IsValidPhoneNumber("5551234", 7), "valid number mid");
    Check(!IsValidPhoneNumber("0000000", 7), "leading zero rejected");
    Check(!IsValidPhoneNumber("100000", 6), "too short");
    Check(!IsValidPhoneNumber("10000001", 8), "too long");
    Check(!IsValidPhoneNumber("100000A", 7), "non-digit rejected");
    Check(!IsValidPhoneNumber("", 0), "empty rejected");

    // ── Client ops range ────────────────────────────────────────────
    Check(IsKnownClientOp(1) && IsKnownClientOp(11), "client ops 1..14 known");
    Check(!IsKnownClientOp(0) && !IsKnownClientOp(15), "client ops outside range unknown");

    // ── Error codes range ───────────────────────────────────────────
    Check(IsKnownErrorCode(1) && IsKnownErrorCode(12), "error codes 1..12 known");
    Check(!IsKnownErrorCode(0) && !IsKnownErrorCode(13), "error codes outside range unknown");

    // ── ClientRequest envelope (from native_api.h) ──────────────────
    {
        uint8_t payload[] = {0x42};
        ClientRequest m;
        m.sessionId = 0xDEAD;
        m.requestId = 7;
        m.domain = RequestDomain::Phone;
        m.op = 1;
        m.payload = payload;
        m.payloadLen = 1;
        const size_t n = EncodeClientRequest(m, buf, sizeof(buf));
        Check(n == kClientRequestHeaderSize + 1, "client request size");
        Check(buf[0] == kEnhancedMarker && buf[1] == 9, "client request envelope");

        ClientRequest d;
        Check(DecodeClientRequest(buf, n, d), "client request decode");
        Check(d.sessionId == 0xDEAD && d.requestId == 7, "client request ids");
        Check(d.domain == RequestDomain::Phone && d.op == 1, "client request domain/op");
        Check(d.payloadLen == 1 && d.payload[0] == 0x42, "client request payload");
    }
    // Reject: zero session
    {
        ClientRequest m; m.requestId = 1; m.sessionId = 0; m.op = 1;
        Check(EncodeClientRequest(m, buf, sizeof(buf)) == 0, "reject zero session");
    }
    // Reject: zero request id
    {
        ClientRequest m; m.sessionId = 1; m.requestId = 0; m.op = 1;
        Check(EncodeClientRequest(m, buf, sizeof(buf)) == 0, "reject zero request id");
    }

    // ── ContactListRequest ──────────────────────────────────────────
    {
        ContactListRequest m; m.page = 3;
        const size_t n = EncodeContactListRequest(m, buf, sizeof(buf));
        Check(n == 1, "contact list req size");
        ContactListRequest d;
        Check(DecodeContactListRequest(buf, n, d), "contact list req decode");
        Check(d.page == 3, "contact list req page");
    }

    // ── ContactAdd ──────────────────────────────────────────────────
    {
        ContactAddRequest m;
        SetStr(m.number, m.numberLen, "1234567");
        SetStr(m.name, m.nameLen, "Ion");
        const size_t n = EncodeContactAdd(m, buf, sizeof(buf));
        Check(n == (1+7) + (1+3), "contact add size");
        ContactAddRequest d;
        Check(DecodeContactAdd(buf, n, d), "contact add decode");
        Check(d.numberLen == 7 && memcmp(d.number, "1234567", 7) == 0, "contact add number");
        Check(d.nameLen == 3 && memcmp(d.name, "Ion", 3) == 0, "contact add name");
    }
    // Reject: invalid number
    {
        ContactAddRequest m;
        SetStr(m.number, m.numberLen, "0000000");
        SetStr(m.name, m.nameLen, "Bad");
        Check(EncodeContactAdd(m, buf, sizeof(buf)) == 0, "contact add reject bad number");
    }
    // Reject: empty name
    {
        ContactAddRequest m;
        SetStr(m.number, m.numberLen, "1234567");
        m.nameLen = 0;
        Check(EncodeContactAdd(m, buf, sizeof(buf)) == 0, "contact add reject empty name");
    }

    // ── ContactRemove ───────────────────────────────────────────────
    {
        ContactRemoveRequest m;
        SetStr(m.number, m.numberLen, "5551234");
        const size_t n = EncodeContactRemove(m, buf, sizeof(buf));
        Check(n == 1+7, "contact remove size");
        ContactRemoveRequest d;
        Check(DecodeContactRemove(buf, n, d), "contact remove decode");
        Check(d.numberLen == 7, "contact remove number len");
    }

    // ── SmsThreadsRequest ───────────────────────────────────────────
    {
        SmsThreadsRequest m; m.page = 2;
        const size_t n = EncodeSmsThreadsRequest(m, buf, sizeof(buf));
        Check(n == 1, "sms threads req size");
        SmsThreadsRequest d;
        Check(DecodeSmsThreadsRequest(buf, n, d), "sms threads req decode");
        Check(d.page == 2, "sms threads req page");
    }

    // ── SmsHistoryRequest ───────────────────────────────────────────
    {
        SmsHistoryRequest m;
        SetStr(m.peerNumber, m.peerNumberLen, "1000042");
        m.page = 1;
        const size_t n = EncodeSmsHistoryReq(m, buf, sizeof(buf));
        Check(n == (1+7) + 1, "sms history req size");
        SmsHistoryRequest d;
        Check(DecodeSmsHistoryReq(buf, n, d), "sms history req decode");
        Check(d.peerNumberLen == 7 && d.page == 1, "sms history req fields");
    }

    // ── SmsSend ─────────────────────────────────────────────────────
    {
        SmsSendRequest m;
        SetStr(m.recipientNumber, m.recipientNumberLen, "1000042");
        SetStr(m.text, m.textLen, "Salut!");
        const size_t n = EncodeSmsSend(m, buf, sizeof(buf));
        Check(n == (1+7) + (1+6), "sms send size");
        SmsSendRequest d;
        Check(DecodeSmsSend(buf, n, d), "sms send decode");
        Check(d.recipientNumberLen == 7, "sms send recipient");
        Check(d.textLen == 6 && memcmp(d.text, "Salut!", 6) == 0, "sms send text");
    }
    // Max-size SMS
    {
        SmsSendRequest m;
        SetStr(m.recipientNumber, m.recipientNumberLen, "1000042");
        m.textLen = 120;
        memset(m.text, 'A', 120); m.text[120] = '\0';
        const size_t n = EncodeSmsSend(m, buf, sizeof(buf));
        Check(n == (1+7) + (1+120), "sms send max size");
        SmsSendRequest d;
        Check(DecodeSmsSend(buf, n, d), "sms send max decode");
        Check(d.textLen == 120, "sms send max text len");
    }

    // ── CallDial ────────────────────────────────────────────────────
    {
        CallDialRequest m;
        SetStr(m.number, m.numberLen, "1000042");
        const size_t n = EncodeCallDial(m, buf, sizeof(buf));
        Check(n == 1+7, "call dial size");
        CallDialRequest d;
        Check(DecodeCallDial(buf, n, d), "call dial decode");
    }

    // ── CallAccept / CallDecline / CallHangup (with callSessionId) ──
    {
        CallAcceptRequest m; m.callSessionId = 17;
        const size_t n = EncodeCallAccept(m, buf, sizeof(buf));
        Check(n == 4, "call accept size");
        CallAcceptRequest d;
        Check(DecodeCallAccept(buf, n, d) && d.callSessionId == 17, "call accept roundtrip");
    }
    {
        CallDeclineRequest m; m.callSessionId = 42;
        const size_t n = EncodeCallDecline(m, buf, sizeof(buf));
        Check(n == 4, "call decline size");
        CallDeclineRequest d;
        Check(DecodeCallDecline(buf, n, d) && d.callSessionId == 42, "call decline roundtrip");
    }
    {
        CallHangupRequest m; m.callSessionId = 99;
        const size_t n = EncodeCallHangup(m, buf, sizeof(buf));
        Check(n == 4, "call hangup size");
        CallHangupRequest d;
        Check(DecodeCallHangup(buf, n, d) && d.callSessionId == 99, "call hangup roundtrip");
    }
    // Reject: zero callSessionId
    {
        CallAcceptRequest m; m.callSessionId = 0;
        Check(EncodeCallAccept(m, buf, sizeof(buf)) == 0, "reject zero callSessionId accept");
        CallDeclineRequest m2; m2.callSessionId = 0;
        Check(EncodeCallDecline(m2, buf, sizeof(buf)) == 0, "reject zero callSessionId decline");
        CallHangupRequest m3; m3.callSessionId = 0;
        Check(EncodeCallHangup(m3, buf, sizeof(buf)) == 0, "reject zero callSessionId hangup");
    }

    // ═══════════════════════════════════════════════════════════════
    // SERVER RESPONSES
    // ═══════════════════════════════════════════════════════════════

    // ── PhoneSync ───────────────────────────────────────────────────
    {
        PhoneSyncResponse m;
        m.requestId = 1;
        SetStr(m.ownNumber, m.ownNumberLen, "1000001");
        m.flags = 0;
        const size_t n = EncodePhoneSync(m, buf, sizeof(buf));
        Check(n == 4 + (1+7) + 1, "phone sync size = 13");
        PhoneSyncResponse d;
        Check(DecodePhoneSync(buf, n, d), "phone sync decode");
        Check(d.requestId == 1 && d.ownNumberLen == 7 && d.flags == 0, "phone sync fields");
    }

    // ── ContactList (boundary: max page) ────────────────────────────
    {
        ContactListResponse m;
        m.requestId = 10;
        m.count = kContactListMaxPerPage;
        m.flags = kFlagHasMore;
        for (uint8_t i = 0; i < m.count; ++i) {
            auto& e = m.entries[i];
            memset(e.name, 'A' + (i % 26), kMaxContactName);
            e.name[kMaxContactName] = '\0';
            e.nameLen = kMaxContactName;
            SetStr(e.number, e.numberLen, "1000001");
            e.hue = i;
        }
        const size_t n = EncodeContactList(m, buf, sizeof(buf));
        Check(n > 0, "contact list max encode");
        Check(n <= kMaxCallPayload, "contact list max fits 400B payload");
        Check(n == 6 + kContactListMaxPerPage * 33, "contact list max exact size");

        ContactListResponse d;
        Check(DecodeContactList(buf, n, d), "contact list max decode");
        Check(d.count == kContactListMaxPerPage, "contact list max count");
        Check(d.flags == kFlagHasMore, "contact list max hasMore");
        Check(d.entries[0].nameLen == kMaxContactName, "contact list max name len");
        Check(d.entries[0].hue == 0, "contact list max hue");
    }
    // Reject: over max page size
    {
        ContactListResponse m;
        m.requestId = 1; m.count = kContactListMaxPerPage + 1;
        Check(EncodeContactList(m, buf, sizeof(buf)) == 0, "contact list over max rejected");
    }
    // Empty page (valid)
    {
        ContactListResponse m;
        m.requestId = 5; m.count = 0; m.flags = 0;
        const size_t n = EncodeContactList(m, buf, sizeof(buf));
        Check(n == 6, "contact list empty = 6 bytes");
        ContactListResponse d;
        Check(DecodeContactList(buf, n, d), "contact list empty decode");
        Check(d.count == 0 && d.flags == 0, "contact list empty fields");
    }

    // ── ContactResult ───────────────────────────────────────────────
    {
        ContactResultResponse m; m.requestId = 3; m.status = ContactResultStatus::Ok;
        const size_t n = EncodeContactResult(m, buf, sizeof(buf));
        Check(n == 5, "contact result size");
        ContactResultResponse d;
        Check(DecodeContactResult(buf, n, d), "contact result decode");
        Check(d.requestId == 3 && d.status == ContactResultStatus::Ok, "contact result fields");
    }

    // ── SmsThreadList (boundary: max page with peerNumber) ──────────
    {
        SmsThreadListResponse m;
        m.requestId = 20;
        m.count = kSmsThreadMaxPerPage;
        m.flags = kFlagHasMore;
        for (uint8_t i = 0; i < m.count; ++i) {
            auto& e = m.entries[i];
            memset(e.peerName, 'B' + i, kMaxPeerName);
            e.peerName[kMaxPeerName] = '\0';
            e.peerNameLen = kMaxPeerName;
            SetStr(e.peerNumber, e.peerNumberLen, "2000001");
            memset(e.preview, 'x', kMaxPreviewText);
            e.preview[kMaxPreviewText] = '\0';
            e.previewLen = kMaxPreviewText;
            e.timestamp = 1729785600u + i;
            e.unread = i;
            e.hue = 100 + i;
        }
        const size_t n = EncodeSmsThreadList(m, buf, sizeof(buf));
        Check(n > 0, "sms thread list max encode");
        Check(n <= kMaxCallPayload, "sms thread list max fits 400B payload");
        Check(n == 6 + kSmsThreadMaxPerPage * 86, "sms thread list max exact size = 350");

        SmsThreadListResponse d;
        Check(DecodeSmsThreadList(buf, n, d), "sms thread list max decode");
        Check(d.count == kSmsThreadMaxPerPage, "sms thread list count");
        Check(d.flags == kFlagHasMore, "sms thread list hasMore");
        Check(d.entries[0].peerNumberLen == 7, "thread has peer number");
        Check(d.entries[0].timestamp == 1729785600u, "thread timestamp");
        Check(d.entries[1].unread == 1, "thread unread");
    }

    // ── SmsHistory (boundary: max page with messageId + direction) ──
    {
        SmsHistoryResponse m;
        m.requestId = 30;
        m.count = kSmsHistoryMaxPerPage;
        m.flags = kFlagHasMore;
        for (uint8_t i = 0; i < m.count; ++i) {
            auto& e = m.entries[i];
            e.messageId = 4000 + i;
            memset(e.text, 'Z', kMaxSmsText);
            e.text[kMaxSmsText] = '\0';
            e.textLen = kMaxSmsText;
            e.timestamp = 1729785600u + i * 60;
            e.delivery = 0;
            e.direction = (i % 2 == 0) ? SmsDirection::Outgoing : SmsDirection::Incoming;
        }
        const size_t n = EncodeSmsHistory(m, buf, sizeof(buf));
        Check(n > 0, "sms history max encode");
        Check(n <= kMaxCallPayload, "sms history max fits 400B payload");
        Check(n == 6 + kSmsHistoryMaxPerPage * 131, "sms history max exact size = 399");

        SmsHistoryResponse d;
        Check(DecodeSmsHistory(buf, n, d), "sms history max decode");
        Check(d.count == kSmsHistoryMaxPerPage, "sms history count");
        Check(d.entries[0].messageId == 4000, "sms history msgId");
        Check(d.entries[0].direction == SmsDirection::Outgoing, "sms history direction 0");
        Check(d.entries[1].direction == SmsDirection::Incoming, "sms history direction 1");
        Check(d.entries[2].timestamp == 1729785600u + 120, "sms history timestamp");
    }
    // Reject: invalid direction
    {
        SmsHistoryResponse m;
        m.requestId = 31; m.count = 1; m.flags = 0;
        m.entries[0].messageId = 1;
        SetStr(m.entries[0].text, m.entries[0].textLen, "Hi");
        m.entries[0].timestamp = 1;
        m.entries[0].delivery = 0;
        m.entries[0].direction = SmsDirection::Outgoing;
        const size_t n = EncodeSmsHistory(m, buf, sizeof(buf));
        Check(n > 0, "sms history single encode");
        buf[n - 1] = 2;  // corrupt direction to invalid value
        SmsHistoryResponse d;
        Check(!DecodeSmsHistory(buf, n, d), "sms history reject bad direction");
    }

    // ── SmsSendResult ───────────────────────────────────────────────
    {
        SmsSendResultResponse m; m.requestId = 40; m.status = SmsSendStatus::Stored; m.messageId = 4217;
        const size_t n = EncodeSmsSendResult(m, buf, sizeof(buf));
        Check(n == 9, "sms send result size");
        SmsSendResultResponse d;
        Check(DecodeSmsSendResult(buf, n, d), "sms send result decode");
        Check(d.status == SmsSendStatus::Stored && d.messageId == 4217, "sms send result fields");
    }

    // ── SmsReceived (push, with timestamp) ──────────────────────────
    {
        SmsReceivedPush m;
        SetStr(m.senderName, m.senderNameLen, "Andrei");
        SetStr(m.senderNumber, m.senderNumberLen, "1000042");
        SetStr(m.text, m.textLen, "Salut!");
        m.timestamp = 1729785600u;
        m.messageId = 4217;
        m.hue = 42;
        const size_t n = EncodeSmsReceived(m, buf, sizeof(buf));
        Check(n > 0, "sms received encode");
        SmsReceivedPush d;
        Check(DecodeSmsReceived(buf, n, d), "sms received decode");
        Check(d.senderNameLen == 6 && memcmp(d.senderName, "Andrei", 6) == 0, "sms received name");
        Check(d.senderNumberLen == 7, "sms received number");
        Check(d.textLen == 6, "sms received text");
        Check(d.timestamp == 1729785600u, "sms received timestamp");
        Check(d.messageId == 4217 && d.hue == 42, "sms received id/hue");
    }
    // SmsReceived max size
    {
        SmsReceivedPush m;
        memset(m.senderName, 'X', kMaxPeerName); m.senderName[kMaxPeerName] = '\0'; m.senderNameLen = kMaxPeerName;
        SetStr(m.senderNumber, m.senderNumberLen, "9999999");
        memset(m.text, 'Y', kMaxSmsText); m.text[kMaxSmsText] = '\0'; m.textLen = kMaxSmsText;
        m.timestamp = 0xFFFFFFFF; m.messageId = 0xFFFFFFFF; m.hue = 255;
        const size_t n = EncodeSmsReceived(m, buf, sizeof(buf));
        Check(n == (1+23) + (1+7) + (1+120) + 4 + 4 + 1, "sms received max = 162");
        Check(n <= kMaxCallPayload, "sms received fits 400B");
    }

    // ── CallState (with callSessionId) ──────────────────────────────
    {
        CallStateResponse m;
        m.requestId = 50; m.callSessionId = 17;
        m.state = CallStatus::Active;
        SetStr(m.peerName, m.peerNameLen, "Mihai");
        SetStr(m.peerNumber, m.peerNumberLen, "1000042");
        m.peerHue = 200; m.startTime = 1729785600u;
        const size_t n = EncodeCallState(m, buf, sizeof(buf));
        Check(n > 0, "call state encode");
        CallStateResponse d;
        Check(DecodeCallState(buf, n, d), "call state decode");
        Check(d.requestId == 50, "call state reqId");
        Check(d.callSessionId == 17, "call state callSessionId");
        Check(d.state == CallStatus::Active, "call state state");
        Check(d.peerNameLen == 5 && memcmp(d.peerName, "Mihai", 5) == 0, "call state peer");
        Check(d.peerHue == 200 && d.startTime == 1729785600u, "call state hue/time");
    }
    // CallState max size
    {
        CallStateResponse m;
        m.requestId = 1; m.callSessionId = 0xFFFFFFFF; m.state = CallStatus::Ended;
        memset(m.peerName, 'A', kMaxPeerName); m.peerName[kMaxPeerName] = '\0'; m.peerNameLen = kMaxPeerName;
        SetStr(m.peerNumber, m.peerNumberLen, "9999999");
        m.peerHue = 255; m.startTime = 0xFFFFFFFF;
        const size_t n = EncodeCallState(m, buf, sizeof(buf));
        Check(n == 4 + 4 + 1 + (1+23) + (1+7) + 1 + 4, "call state max = 46");
        Check(n <= kMaxCallPayload, "call state fits 400B");
    }
    // Reject: invalid call status
    {
        CallStateResponse m;
        m.requestId = 1; m.callSessionId = 1; m.state = CallStatus::Idle;
        SetStr(m.peerName, m.peerNameLen, "A");
        SetStr(m.peerNumber, m.peerNumberLen, "1000001");
        const size_t n = EncodeCallState(m, buf, sizeof(buf));
        Check(n > 0, "call state idle encode");
        buf[8] = 6;  // corrupt state byte to value > 5
        CallStateResponse d;
        Check(!DecodeCallState(buf, n, d), "call state reject bad status");
    }

    // ── PhoneError ──────────────────────────────────────────────────
    {
        PhoneErrorResponse m;
        m.requestId = 60; m.code = ErrorCode::StaleCall;
        SetStr(m.message, m.messageLen, "Call already ended");
        const size_t n = EncodePhoneError(m, buf, sizeof(buf));
        Check(n > 0, "phone error encode");
        PhoneErrorResponse d;
        Check(DecodePhoneError(buf, n, d), "phone error decode");
        Check(d.requestId == 60, "phone error reqId");
        Check(d.code == ErrorCode::StaleCall, "phone error code STALE_CALL");
        Check(d.messageLen == 18, "phone error message len");
    }
    // PhoneError max size
    {
        PhoneErrorResponse m;
        m.requestId = 1; m.code = ErrorCode::InternalError;
        memset(m.message, 'E', kMaxErrorMessage); m.message[kMaxErrorMessage] = '\0'; m.messageLen = kMaxErrorMessage;
        const size_t n = EncodePhoneError(m, buf, sizeof(buf));
        Check(n == 4 + 1 + (1+64), "phone error max = 70");
    }
    // Reject: unknown error code
    {
        PhoneErrorResponse m;
        m.requestId = 1; m.code = ErrorCode::InternalError;
        SetStr(m.message, m.messageLen, "x");
        const size_t n = EncodePhoneError(m, buf, sizeof(buf));
        Check(n > 0, "phone error encode for corruption");
        buf[4] = 13;  // corrupt error code to invalid
        PhoneErrorResponse d;
        Check(!DecodePhoneError(buf, n, d), "phone error reject bad code");
    }

    // ── UTF-8 / Romanian diacritics pass through ────────────────────
    {
        ContactAddRequest m;
        SetStr(m.number, m.numberLen, "1000001");
        const char* name = "Ioni\xC8\x9B\xC4\x83";  // "Ioniță" in UTF-8 (ț=C899, ă=C483)
        m.nameLen = static_cast<uint8_t>(strlen(name));
        memcpy(m.name, name, m.nameLen); m.name[m.nameLen] = '\0';
        const size_t n = EncodeContactAdd(m, buf, sizeof(buf));
        Check(n > 0, "utf8 diacritic name encodes");
        ContactAddRequest d;
        Check(DecodeContactAdd(buf, n, d), "utf8 diacritic name decodes");
        Check(d.nameLen == m.nameLen && memcmp(d.name, name, m.nameLen) == 0, "utf8 diacritic preserved");
    }

    // ── Page size constants are sound ───────────────────────────────
    Check(6 + kContactListMaxPerPage * 33 <= kMaxCallPayload, "contact page fits");
    Check(6 + (kContactListMaxPerPage + 1) * 33 > kMaxCallPayload, "contact page+1 overflows");
    Check(6 + kSmsThreadMaxPerPage * 86 <= kMaxCallPayload, "thread page fits");
    Check(6 + (kSmsThreadMaxPerPage + 1) * 86 > kMaxCallPayload, "thread page+1 overflows");
    Check(6 + kSmsHistoryMaxPerPage * 131 <= kMaxCallPayload, "history page fits");
    Check(6 + (kSmsHistoryMaxPerPage + 1) * 131 > kMaxCallPayload, "history page+1 overflows");

    std::cout << "OK: " << checks << " checks passed.\n";
    return 0;
}
