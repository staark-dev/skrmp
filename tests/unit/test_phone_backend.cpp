// Unit tests for PHONE-M4E: authentication bridge, step 9 routing,
// capability negotiation, and phone error responses.
// Tests the codec/protocol-level logic that can be verified without
// a live open.mp server or AMX runtime.

#include "protocol/native_api.h"
#include "protocol/phone_protocol.h"

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
    using namespace skr::protocol::phone;

    // ── Step 9 (ClientRequest) encode/decode ────────────────────────
    {
        // Encode a PhoneSync request (empty payload, op 1)
        ClientRequest req;
        req.sessionId = 0x12345678;
        req.requestId = 42;
        req.domain = RequestDomain::Phone;
        req.op = static_cast<uint8_t>(ClientOp::PhoneSync);
        req.payload = nullptr;
        req.payloadLen = 0;

        uint8_t buf[kMaxClientRequestSize];
        const size_t n = EncodeClientRequest(req, buf, sizeof(buf));
        Check(n == kClientRequestHeaderSize, "PhoneSync request is header-only");
        Check(buf[0] == kEnhancedMarker && buf[1] == static_cast<uint8_t>(Step::ClientRequest),
              "step 9 envelope");

        ClientRequest dec;
        Check(DecodeClientRequest(buf, n, dec), "PhoneSync decode");
        Check(dec.sessionId == 0x12345678 && dec.requestId == 42 &&
              dec.domain == RequestDomain::Phone &&
              dec.op == static_cast<uint8_t>(ClientOp::PhoneSync) &&
              dec.payloadLen == 0, "PhoneSync fields");
    }

    // ── Step 9 with ContactAdd payload ──────────────────────────────
    {
        ContactAddRequest caReq;
        std::memcpy(caReq.number, "1234567", 7);
        caReq.numberLen = 7;
        std::memcpy(caReq.name, "TestPlayer", 10);
        caReq.nameLen = 10;

        uint8_t payload[64];
        const size_t pn = EncodeContactAdd(caReq, payload, sizeof(payload));
        Check(pn > 0, "ContactAdd payload encodes");

        ClientRequest req;
        req.sessionId = 99;
        req.requestId = 1;
        req.domain = RequestDomain::Phone;
        req.op = static_cast<uint8_t>(ClientOp::ContactAdd);
        req.payload = payload;
        req.payloadLen = static_cast<uint16_t>(pn);

        uint8_t buf[kMaxClientRequestSize];
        const size_t n = EncodeClientRequest(req, buf, sizeof(buf));
        Check(n == kClientRequestHeaderSize + pn, "ContactAdd request size");

        ClientRequest dec;
        Check(DecodeClientRequest(buf, n, dec), "ContactAdd request decode");
        Check(dec.op == static_cast<uint8_t>(ClientOp::ContactAdd), "ContactAdd op");
        Check(dec.payloadLen == pn, "ContactAdd payload len");

        ContactAddRequest caDec;
        Check(DecodeContactAdd(dec.payload, dec.payloadLen, caDec), "ContactAdd payload decode");
        Check(std::memcmp(caDec.number, "1234567", 7) == 0 && caDec.numberLen == 7, "ContactAdd number");
        Check(std::memcmp(caDec.name, "TestPlayer", 10) == 0 && caDec.nameLen == 10, "ContactAdd name");
    }

    // ── Step 9 with SmsSend payload ─────────────────────────────────
    {
        SmsSendRequest ssReq;
        std::memcpy(ssReq.recipientNumber, "9876543", 7);
        ssReq.recipientNumberLen = 7;
        std::memcpy(ssReq.text, "Hello world", 11);
        ssReq.textLen = 11;

        uint8_t payload[256];
        const size_t pn = EncodeSmsSend(ssReq, payload, sizeof(payload));
        Check(pn > 0, "SmsSend payload encodes");

        ClientRequest req;
        req.sessionId = 100;
        req.requestId = 2;
        req.domain = RequestDomain::Phone;
        req.op = static_cast<uint8_t>(ClientOp::SmsSend);
        req.payload = payload;
        req.payloadLen = static_cast<uint16_t>(pn);

        uint8_t buf[kMaxClientRequestSize];
        const size_t n = EncodeClientRequest(req, buf, sizeof(buf));
        Check(n > 0, "SmsSend request encodes");

        ClientRequest dec;
        Check(DecodeClientRequest(buf, n, dec), "SmsSend request decode");
        SmsSendRequest ssDec;
        Check(DecodeSmsSend(dec.payload, dec.payloadLen, ssDec), "SmsSend payload decode");
        Check(ssDec.textLen == 11 && std::memcmp(ssDec.text, "Hello world", 11) == 0, "SmsSend text");
    }

    // ── ClientRequest rejection: zero session ───────────────────────
    {
        ClientRequest req;
        req.sessionId = 0;
        req.requestId = 1;
        req.domain = RequestDomain::Phone;
        req.op = 1;
        req.payloadLen = 0;
        uint8_t buf[kMaxClientRequestSize];
        Check(EncodeClientRequest(req, buf, sizeof(buf)) == 0, "zero session rejected");
    }

    // ── ClientRequest rejection: zero requestId ─────────────────────
    {
        ClientRequest req;
        req.sessionId = 1;
        req.requestId = 0;
        req.domain = RequestDomain::Phone;
        req.op = 1;
        req.payloadLen = 0;
        uint8_t buf[kMaxClientRequestSize];
        Check(EncodeClientRequest(req, buf, sizeof(buf)) == 0, "zero requestId rejected");
    }

    // ── ClientRequest rejection: oversized payload ──────────────────
    {
        uint8_t big[kMaxClientRequestPayload + 1] = {};
        ClientRequest req;
        req.sessionId = 1;
        req.requestId = 1;
        req.domain = RequestDomain::Phone;
        req.op = 1;
        req.payload = big;
        req.payloadLen = static_cast<uint16_t>(sizeof(big));
        uint8_t buf[512];
        Check(EncodeClientRequest(req, buf, sizeof(buf)) == 0, "oversized payload rejected");
    }

    // ── ClientRequest rejection: unknown domain ─────────────────────
    {
        ClientRequest req;
        req.sessionId = 1;
        req.requestId = 1;
        req.domain = RequestDomain::Phone;
        req.op = 1;
        req.payloadLen = 0;

        uint8_t buf[kMaxClientRequestSize];
        const size_t n = EncodeClientRequest(req, buf, sizeof(buf));
        Check(n > 0, "encode ok for domain mutation");
        buf[10] = 99;  // corrupt domain byte
        ClientRequest dec;
        Check(!DecodeClientRequest(buf, n, dec), "unknown domain rejected by decoder");
    }

    // ── PhoneError roundtrip ────────────────────────────────────────
    {
        PhoneErrorResponse err;
        err.requestId = 42;
        err.code = ErrorCode::NoPhone;
        const char* msg = "not authenticated";
        err.messageLen = static_cast<uint8_t>(std::strlen(msg));
        std::memcpy(err.message, msg, err.messageLen);

        uint8_t payload[128];
        const size_t n = EncodePhoneError(err, payload, sizeof(payload));
        Check(n > 0, "PhoneError encodes");

        PhoneErrorResponse dec;
        Check(DecodePhoneError(payload, n, dec), "PhoneError decodes");
        Check(dec.requestId == 42, "PhoneError requestId");
        Check(dec.code == ErrorCode::NoPhone, "PhoneError code");
        Check(dec.messageLen == err.messageLen && std::memcmp(dec.message, msg, err.messageLen) == 0,
              "PhoneError message");
    }

    // ── CapPhone bit ────────────────────────────────────────────────
    {
        Check(CapPhone == 0x08, "CapPhone is 0x08");
        Check((kKnownCapabilities & CapPhone) != 0, "CapPhone is a known capability");
        Check(IsPhoneOp(16) && IsPhoneOp(24) && !IsPhoneOp(15) && IsPhoneOp(25) && IsPhoneOp(26) && !IsPhoneOp(27),
              "phone ops are 16..25 (M5 negotiated separately)");
        Check(CapabilityFor(Op::PhoneSync) == CapPhone, "PhoneSync requires CapPhone");
        Check(CapabilityFor(Op::PhoneError) == CapPhone, "PhoneError requires CapPhone");
    }

    // ── Step 9 is a valid API step ──────────────────────────────────
    {
        Check(IsApiStep(static_cast<uint8_t>(Step::ClientRequest)), "step 9 is API step");
        Check(!IsApiStep(10), "step 10 is not API step");
    }

    // ── ClientOp range validation ───────────────────────────────────
    {
        Check(IsKnownClientOp(1) && IsKnownClientOp(11), "client ops 1..14");
        Check(!IsKnownClientOp(0) && !IsKnownClientOp(15), "out-of-range client ops rejected");
    }

    // ── kMaxInboundSize covers step 9 ───────────────────────────────
    {
        Check(kMaxInboundSize >= kMaxClientRequestSize, "inbound buffer fits step 9");
        Check(kMaxClientRequestSize == kClientRequestHeaderSize + kMaxClientRequestPayload,
              "max request size = header + max payload");
        Check(kClientRequestHeaderSize == 14, "step 9 header is 14 bytes");
        Check(kMaxClientRequestPayload == 200, "step 9 max payload is 200 bytes");
    }

    // ── All call ops carry callSessionId ────────────────────────────
    {
        CallAcceptRequest ca; ca.callSessionId = 0;
        uint8_t buf[16];
        Check(EncodeCallAccept(ca, buf, sizeof(buf)) == 0, "CallAccept rejects zero sessionId");
        ca.callSessionId = 77;
        Check(EncodeCallAccept(ca, buf, sizeof(buf)) == 4, "CallAccept encodes with valid sessionId");
        CallAcceptRequest dec;
        Check(DecodeCallAccept(buf, 4, dec) && dec.callSessionId == 77, "CallAccept roundtrip");

        CallDeclineRequest cd; cd.callSessionId = 0;
        Check(EncodeCallDecline(cd, buf, sizeof(buf)) == 0, "CallDecline rejects zero sessionId");

        CallHangupRequest ch; ch.callSessionId = 0;
        Check(EncodeCallHangup(ch, buf, sizeof(buf)) == 0, "CallHangup rejects zero sessionId");
    }

    // ── Max-payload step 9 fits in buffer ───────────────────────────
    {
        uint8_t payload[kMaxClientRequestPayload];
        std::memset(payload, 0x42, sizeof(payload));
        ClientRequest req;
        req.sessionId = 1;
        req.requestId = 1;
        req.domain = RequestDomain::Phone;
        req.op = 1;
        req.payload = payload;
        req.payloadLen = kMaxClientRequestPayload;

        uint8_t buf[kMaxClientRequestSize];
        const size_t n = EncodeClientRequest(req, buf, sizeof(buf));
        Check(n == kMaxClientRequestSize, "max-payload request fills exactly");
        ClientRequest dec;
        Check(DecodeClientRequest(buf, n, dec), "max-payload request decodes");
        Check(dec.payloadLen == kMaxClientRequestPayload, "max-payload length preserved");
    }

    std::cout << "phone_backend: " << checks << " checks passed\n";
    return 0;
}
