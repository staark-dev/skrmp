#pragma once
#include "protocol/skrx_gateway.h"
#include "protocol/phone_protocol.h"
#include "protocol/phone_profile.h"

namespace skr::protocol::skrx::phone {
// Project-local PHONE v1. Existing domain codecs are reused, not nested API envelopes.
constexpr uint8_t kVersion = 1;
constexpr size_t kResponseHeader = 5; // op, sessionId
constexpr size_t kRequestHeader = 9;  // op, sessionId, requestId
constexpr uint8_t kHistoryPageSize = 2; // 5 + 6 + 2*131 <= gateway's 400 bytes
enum class Transport : uint8_t { Disabled, Legacy, GatewayV1 };
inline uint32_t NormalizeCapabilities(uint32_t caps, bool enabled=true, bool gatewayEnabled=true) {
    if (!gatewayEnabled) caps &= ~api::CapPhoneGateway;
    if (!enabled) caps &= ~(api::CapPhone | api::CapPhoneSetup | api::CapPhoneGateway);
    if (!(caps & api::CapPhone)) caps &= ~(api::CapPhoneSetup | api::CapPhoneGateway);
    if (!(caps & api::CapGateway)) caps &= ~api::CapPhoneGateway;
    return caps;
}
inline Transport SelectTransport(uint32_t caps) {
    caps=NormalizeCapabilities(caps);
    return !(caps & api::CapPhone)?Transport::Disabled
        : (caps & api::CapPhoneGateway)?Transport::GatewayV1:Transport::Legacy;
}
inline bool ValidateRequest(uint8_t op,const uint8_t* p,size_t n,bool setup) {
    namespace ph=protocol::phone;
    if (n && !p) return false;
    switch (static_cast<ph::ClientOp>(op)) {
    case ph::ClientOp::PhoneSync: return n==0;
    case ph::ClientOp::ContactList: { ph::ContactListRequest x; return ph::DecodeContactListRequest(p,n,x); }
    case ph::ClientOp::ContactAdd: { ph::ContactAddRequest x; return ph::DecodeContactAdd(p,n,x); }
    case ph::ClientOp::ContactRemove: { ph::ContactRemoveRequest x; return ph::DecodeContactRemove(p,n,x); }
    case ph::ClientOp::SmsThreads: { ph::SmsThreadsRequest x; return ph::DecodeSmsThreadsRequest(p,n,x); }
    case ph::ClientOp::SmsHistory: { ph::SmsHistoryRequest x; return ph::DecodeSmsHistoryReq(p,n,x); }
    case ph::ClientOp::SmsSend: { ph::SmsSendRequest x; return ph::DecodeSmsSend(p,n,x); }
    case ph::ClientOp::ProfileGet: return setup && n==0;
    case ph::ClientOp::ProfileSave: { ph::Profile x; return setup && ph::DecodeProfile(p,n,x) && !x.requestId; }
    case ph::ClientOp::MarkRead: {
        api::Reader r(p,n); char peer[8]; uint8_t len=0;
        if (!setup || !r.Text8(peer,7,len)) return false;
        const auto last=r.U32(); return r.Done() && last && ph::IsValidPhoneNumber(peer,len);
    }
    default: return false; // calls still unsupported
    }
}
inline bool ValidateResponse(uint8_t op,const uint8_t* p,size_t n,bool setup) {
    namespace ph=protocol::phone;
    if (n && !p) return false;
    switch (static_cast<api::Op>(op)) {
    case api::Op::PhoneSync: { ph::PhoneSyncResponse x; return ph::DecodePhoneSync(p,n,x); }
    case api::Op::ContactList: { ph::ContactListResponse x; return ph::DecodeContactList(p,n,x); }
    case api::Op::ContactResult: { ph::ContactResultResponse x; return ph::DecodeContactResult(p,n,x); }
    case api::Op::SmsThreadList: { ph::SmsThreadListResponse x; return ph::DecodeSmsThreadList(p,n,x); }
    case api::Op::SmsHistory: { ph::SmsHistoryResponse x; return ph::DecodeSmsHistory(p,n,x) && x.count<=kHistoryPageSize; }
    case api::Op::SmsSendResult: { ph::SmsSendResultResponse x; return ph::DecodeSmsSendResult(p,n,x); }
    case api::Op::SmsReceived: { ph::SmsReceivedPush x; return ph::DecodeSmsReceived(p,n,x); }
    case api::Op::PhoneError: { ph::PhoneErrorResponse x; return ph::DecodePhoneError(p,n,x); }
    case api::Op::PhoneProfile: { ph::Profile x; return setup && ph::DecodeProfile(p,n,x); }
    case api::Op::PhoneReadResult: { api::Reader r(p,n); const auto id=r.U32(); return setup && id && r.Done(); }
    default: return false;
    }
}
struct Frame {
    uint8_t op=0; uint32_t sessionId=0,requestId=0;
    const uint8_t* body=nullptr; size_t bodyLen=0;
};
inline bool Decode(const GatewayHeader& hdr,const uint8_t* p,size_t n,bool request,bool setup,Frame& f) {
    const auto header=request?kRequestHeader:kResponseHeader;
    if (hdr.ns!=Namespace::Phone || hdr.version!=kVersion || hdr.payloadLen!=n || n<header || n>kMaxGatewayPayload || !p) return false;
    api::Reader r(p,n); f.op=r.U8(); f.sessionId=r.U32(); f.requestId=request?r.U32():0;
    f.body=p+header; f.bodyLen=n-header;
    if (!f.sessionId || (request && !f.requestId)) return false;
    return request ? f.bodyLen<=api::kMaxClientRequestPayload && ValidateRequest(f.op,f.body,f.bodyLen,setup)
                   : ValidateResponse(f.op,f.body,f.bodyLen,setup);
}
inline size_t Encode(uint8_t op,uint32_t sessionId,uint32_t requestId,const uint8_t* body,size_t len,
                     bool request,bool setup,uint8_t* out,size_t cap) {
    const auto header=request?kRequestHeader:kResponseHeader;
    if (!sessionId || (request && !requestId) || len>kMaxGatewayPayload-header || (len && !body)) return 0;
    if (request ? !ValidateRequest(op,body,len,setup) : !ValidateResponse(op,body,len,setup)) return 0;
    uint8_t payload[kMaxGatewayPayload]; api::Writer w(payload,sizeof(payload));
    w.U8(op); w.U32(sessionId); if (request) w.U32(requestId);
    if (len) std::memcpy(payload+header,body,len);
    GatewayHeader hdr; hdr.ns=Namespace::Phone; hdr.version=kVersion; hdr.payloadLen=static_cast<uint16_t>(header+len);
    return EncodeGateway(hdr,payload,out,cap);
}
}
