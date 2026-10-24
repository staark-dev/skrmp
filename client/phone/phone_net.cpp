// PHONE-M4F — Phone network layer (client side).
// Bridges the phone UI (Model) with the server via the Native API envelope.

#include "phone/phone_net.h"
#include "phone/phone_input.h"
#include "phone/phone_model.h"
#include "game-state/game_state.h"
#include "native-api/native_api_ui.h"
#include "protocol/native_api.h"
#include "protocol/phone_protocol.h"
#include "protocol/skrx_phone.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace skr::phone::net {
namespace {

namespace api   = protocol::api;
namespace proto = protocol::phone;

constexpr int kMaxLogLines = 200;
constexpr uint64_t kSendIntervalMs = 120;     // <= 9 requests/s (Gate limit is 10/s)
constexpr uint64_t kTimeoutMs = 8000;
constexpr int kQueueSize = 12;
constexpr int kMaxSmsInFlight = 4;
constexpr uint8_t kAutoHistoryPages = 5;      // 15 messages fetched on open; older on demand

struct Outgoing {
    proto::ClientOp op = proto::ClientOp::PhoneSync;
    uint32_t reqId = 0;
    uint8_t len = 0;
    uint8_t payload[api::kMaxClientRequestPayload] = {};
};

// One expected response per list kind; anything else for that kind is stale.
struct Track {
    uint32_t reqId = 0;
    uint64_t at = 0;
    uint8_t page = 0;
    bool Busy() const { return reqId != 0; }
    void Clear() { reqId = 0; at = 0; }
};

struct SmsInFlight {
    uint32_t reqId = 0;
    uint64_t at = 0;
};

uint32_t g_sessionId = 0;
uint32_t g_nextReqId = 1;
bool     g_active    = false;
protocol::skrx::phone::Transport g_transport=protocol::skrx::phone::Transport::Disabled;
int      g_logLines  = 0;

Outgoing g_queue[kQueueSize];
int      g_qHead = 0, g_qCount = 0;
uint64_t g_lastSendAt = 0;

Track g_sync, g_contacts, g_threads, g_history, g_contactOp, g_profile, g_read;
uint32_t g_receivedIds[128] = {};
size_t g_receivedPos = 0;
uint32_t g_lastReadQueued=0;
char g_readPeer[16]={};
char  g_historyPeer[proto::kPhoneNumberLen + 1] = {};
SmsInFlight g_sms[kMaxSmsInFlight];

// No disk I/O or message/identity contents from phone packet/game callbacks.
void Log(const char* fmt, ...) { (void)fmt; if (g_logLines<kMaxLogLines) ++g_logLines; }

Model& M() { return input::GetModel(); }

void Notice(const char* text, bool error) {
    Model& m = M();
    CopyText(m.notice, text);
    m.noticeError = error;
    m.noticeAt = GetTickCount64();
}

const char* ErrorText(proto::ErrorCode c) {
    switch (c) {
        case proto::ErrorCode::InvalidNumber:   return "Number not found";
        case proto::ErrorCode::Offline:         return "Recipient is offline";
        case proto::ErrorCode::Busy:            return "Line busy";
        case proto::ErrorCode::SelfAction:      return "That is your own number";
        case proto::ErrorCode::RateLimited:     return "Too many requests, try again";
        case proto::ErrorCode::NoPhone:         return "Log in to use your phone";
        case proto::ErrorCode::ContactExists:   return "Contact already saved";
        case proto::ErrorCode::ContactNotFound: return "Contact not found";
        case proto::ErrorCode::ContactsFull:    return "Contact list is full";
        case proto::ErrorCode::SmsTooLong:      return "Message is too long";
        case proto::ErrorCode::StaleCall:       return "Call ended";
        default:                                return "Phone service error";
    }
}

template <size_t N>
void CopyN(char (&dst)[N], const char* src, size_t len) {
    const size_t n = (std::min)(len, N - 1);
    std::memcpy(dst, src, n);
    dst[n] = '\0';
}

// "HH:MM" for today, "DD.MM" otherwise (local time).
void FormatTime(uint32_t ts, char (&out)[8]) {
    if (!ts) { CopyText(out, ""); return; }
    const time_t t = static_cast<time_t>(ts), now = time(nullptr);
    tm lt = {}, ln = {};
    if (localtime_s(&lt, &t) != 0 || localtime_s(&ln, &now) != 0) { CopyText(out, "--:--"); return; }
    if (lt.tm_year == ln.tm_year && lt.tm_yday == ln.tm_yday)
        snprintf(out, sizeof(out), "%02d:%02d", lt.tm_hour, lt.tm_min);
    else
        snprintf(out, sizeof(out), "%02d.%02d", lt.tm_mday, lt.tm_mon + 1);
}

// ── Send queue ──────────────────────────────────────────────────────

uint32_t Enqueue(proto::ClientOp op, const uint8_t* payload, size_t len) {
    if (!g_active || !g_sessionId || len > api::kMaxClientRequestPayload) return 0;
    if (g_qCount >= kQueueSize) { Log("queue full, op=%u dropped", static_cast<unsigned>(op)); return 0; }
    Outgoing& o = g_queue[(g_qHead + g_qCount) % kQueueSize];
    o.op = op;
    o.reqId = g_nextReqId++;
    if (!g_nextReqId) g_nextReqId = 1;
    o.len = static_cast<uint8_t>(len);
    if (len) std::memcpy(o.payload, payload, len);
    ++g_qCount;
    return o.reqId;
}

bool SendNow(const Outgoing& o) {
    if (g_transport==protocol::skrx::phone::Transport::GatewayV1) {
        uint8_t buf[protocol::skrx::kMaxGatewaySize];
        const auto n=protocol::skrx::phone::Encode(static_cast<uint8_t>(o.op),g_sessionId,o.reqId,o.payload,o.len,true,M().setupSupported,buf,sizeof(buf));
        static_assert(protocol::skrx::kGatewayHeaderSize+protocol::skrx::phone::kRequestHeader+api::kMaxClientRequestPayload<=255,"existing RakNet send bound");
        return n && skr::game_state::SendEnhancedEnvelope(buf,n);
    }
    if (g_transport!=protocol::skrx::phone::Transport::Legacy) return false;
    api::ClientRequest req;
    req.sessionId  = g_sessionId;
    req.requestId  = o.reqId;
    req.domain     = api::RequestDomain::Phone;
    req.op         = static_cast<uint8_t>(o.op);
    req.payload    = o.payload;
    req.payloadLen = o.len;
    uint8_t buf[api::kMaxClientRequestSize];
    const size_t n = api::EncodeClientRequest(req, buf, sizeof(buf));
    if (!n) { Log("encode failed op=%u", static_cast<unsigned>(o.op)); return false; }
    const bool sent = skr::game_state::SendEnhancedEnvelope(buf, n);
    Log("send op=%u reqId=%u len=%zu %s", static_cast<unsigned>(o.op), o.reqId, n, sent ? "OK" : "FAIL");
    return sent;
}

// ── List requests ───────────────────────────────────────────────────

bool QueueContacts(uint8_t page) {
    proto::ContactListRequest req;
    req.page = page;
    uint8_t buf[4];
    const size_t n = proto::EncodeContactListRequest(req, buf, sizeof(buf));
    const uint32_t id = n ? Enqueue(proto::ClientOp::ContactList, buf, n) : 0;
    if (!id) return false;
    g_contacts = {id, GetTickCount64(), page};
    M().contactsLoading = true;
    return true;
}

bool QueueThreads(uint8_t page) {
    proto::SmsThreadsRequest req;
    req.page = page;
    uint8_t buf[4];
    const size_t n = proto::EncodeSmsThreadsRequest(req, buf, sizeof(buf));
    const uint32_t id = n ? Enqueue(proto::ClientOp::SmsThreads, buf, n) : 0;
    if (!id) return false;
    g_threads = {id, GetTickCount64(), page};
    M().threadsLoading = true;
    return true;
}

bool QueueHistory(uint8_t page) {
    proto::SmsHistoryRequest req;
    req.peerNumberLen = static_cast<uint8_t>(proto::kPhoneNumberLen);
    std::memcpy(req.peerNumber, g_historyPeer, proto::kPhoneNumberLen);
    req.page = page;
    uint8_t buf[16];
    const size_t n = proto::EncodeSmsHistoryReq(req, buf, sizeof(buf));
    const uint32_t id = n ? Enqueue(proto::ClientOp::SmsHistory, buf, n) : 0;
    if (!id) return false;
    g_history = {id, GetTickCount64(), page};
    M().convo.loading = true;
    return true;
}

// ── Incoming phone ops ──────────────────────────────────────────────

void HandlePhoneSync(const uint8_t* p, size_t n) {
    proto::PhoneSyncResponse r;
    if (!proto::DecodePhoneSync(p, n, r)) { Log("bad PhoneSync payload"); return; }
    // requestId 0 = server push after login; otherwise it must be ours.
    if (r.requestId && r.requestId != g_sync.reqId) { Log("stale PhoneSync reqId=%u", r.requestId); return; }
    g_sync.Clear();
    Model& m = M();
    const bool first = m.sync != SyncState::Ready;
    CopyN(m.ownNumber, r.ownNumber, r.ownNumberLen);
    m.sync = SyncState::Ready;
    m.syncError[0] = 0;
    m.previewData = false;
    Log("PhoneSync: number=%s%s", m.ownNumber, r.requestId ? "" : " (push)");
    if (m.setupSupported && !m.profileLoaded) RequestProfile();
    if (first) {
        QueueContacts(0);
        QueueThreads(0);
    }
}

void HandleContactList(const uint8_t* p, size_t n) {
    proto::ContactListResponse r;
    if (!proto::DecodeContactList(p, n, r)) { Log("bad ContactList payload"); return; }
    if (r.requestId != g_contacts.reqId) { Log("stale ContactList reqId=%u", r.requestId); return; }
    const uint8_t page = g_contacts.page;
    g_contacts.Clear();
    Model& m = M();
    if (page == 0) m.contactCount = 0;
    for (uint8_t i = 0; i < r.count && m.contactCount < kMaxContacts; ++i) {
        const auto& src = r.entries[i];
        Contact& dst = m.contacts[m.contactCount++];
        CopyN(dst.name, src.name, src.nameLen);
        CopyN(dst.number, src.number, src.numberLen);
        dst.hue = src.hue;
    }
    const bool more = (r.flags & proto::kFlagHasMore) && m.contactCount < kMaxContacts && page < 0xFF;
    if (!(more && QueueContacts(static_cast<uint8_t>(page + 1)))) m.contactsLoading = false;
    Log("ContactList: page=%u count=%u total=%d more=%d", page, r.count, m.contactCount, more ? 1 : 0);
}

void HandleContactResult(const uint8_t* p, size_t n) {
    proto::ContactResultResponse r;
    if (!proto::DecodeContactResult(p, n, r)) { Log("bad ContactResult payload"); return; }
    if (r.requestId != g_contactOp.reqId) { Log("stale ContactResult reqId=%u", r.requestId); return; }
    g_contactOp.Clear();
    Model& m = M();
    m.contactOpPending = false;
    const bool ok = r.status == proto::ContactResultStatus::Ok;
    m.contactOpResult = ok ? Model::OpResult::Ok : Model::OpResult::Failed;
    Notice(ok ? "Contacts updated" : "Contact change failed", !ok);
    Log("ContactResult: reqId=%u status=%u", r.requestId, static_cast<unsigned>(r.status));
    if (ok) {
        QueueContacts(0);
        QueueThreads(0);                      // thread names come from contacts
    }
}

void HandleSmsThreadList(const uint8_t* p, size_t n) {
    proto::SmsThreadListResponse r;
    if (!proto::DecodeSmsThreadList(p, n, r)) { Log("bad SmsThreadList payload"); return; }
    if (r.requestId != g_threads.reqId) { Log("stale SmsThreadList reqId=%u", r.requestId); return; }
    const uint8_t page = g_threads.page;
    g_threads.Clear();
    Model& m = M();

    // M4 servers do not track read state; keep their client-side unread
    // counters (bumped by SmsReceived) across refreshes, keyed by number.
    struct Unread { char number[16]; uint8_t count; };
    Unread keep[kMaxThreads];
    int keepCount = 0;
    for (int i = 0; i < m.threadCount; ++i)
        if (m.threads[i].unread) {
            CopyText(keep[keepCount].number, m.threads[i].number);
            keep[keepCount++].count = m.threads[i].unread;
        }

    if (page == 0) m.threadCount = 0;
    for (uint8_t i = 0; i < r.count && m.threadCount < kMaxThreads; ++i) {
        const auto& src = r.entries[i];
        ThreadSummary& dst = m.threads[m.threadCount++];
        CopyN(dst.peer, src.peerName, src.peerNameLen);
        CopyN(dst.number, src.peerNumber, src.peerNumberLen);
        CopyN(dst.preview, src.preview, src.previewLen);
        FormatTime(src.timestamp, dst.time);
        dst.unread = src.unread;
        dst.hue = src.hue;
        for (int k = 0; k < keepCount; ++k)
            if (!m.setupSupported && std::strcmp(keep[k].number, dst.number) == 0) dst.unread = keep[k].count;
    }
    const bool more = (r.flags & proto::kFlagHasMore) && m.threadCount < kMaxThreads && page < 0xFF;
    m.threadsHasMore = (r.flags & proto::kFlagHasMore) != 0;
    if (!(more && QueueThreads(static_cast<uint8_t>(page + 1)))) m.threadsLoading = false;
    Log("SmsThreadList: page=%u count=%u total=%d", page, r.count, m.threadCount);
}

void HandleSmsHistory(const uint8_t* p, size_t n) {
    proto::SmsHistoryResponse r;
    if (!proto::DecodeSmsHistory(p, n, r)) { Log("bad SmsHistory payload"); return; }
    if (r.requestId != g_history.reqId) { Log("stale SmsHistory reqId=%u", r.requestId); return; }
    const uint8_t page = g_history.page;
    g_history.Clear();
    Conversation& cv = M().convo;
    if (std::strncmp(cv.number, g_historyPeer, proto::kPhoneNumberLen) != 0) return;   // switched away

    // Server pages are newest-first; the conversation is oldest-first.
    Message fetched[proto::kSmsHistoryMaxPerPage];
    int fc = 0;
    for (int i = r.count - 1; i >= 0; --i) {
        const auto& src = r.entries[i];
        Message& dst = fetched[fc++];
        dst = Message();
        CopyN(dst.text, src.text, src.textLen);
        FormatTime(src.timestamp, dst.time);
        dst.id = src.messageId;
        dst.delivery = src.direction == proto::SmsDirection::Incoming ? Delivery::Received : Delivery::Delivered;
    }

    // History may race a live push or shift while a new message is inserted.
    // Merge by durable server id instead of replacing the page (which lost
    // live messages) or blindly prepending (which duplicated overlapping pages).
    Message merged[kMaxMessages + proto::kSmsHistoryMaxPerPage];
    int count=0;
    for (int i=0;i<cv.count;++i) if (cv.items[i].id) merged[count++]=cv.items[i];
    for (int i=0;i<fc;++i) {
        bool duplicate=false;
        for (int j=0;j<count;++j) if (merged[j].id==fetched[i].id) { duplicate=true; break; }
        if (!duplicate) merged[count++]=fetched[i];
    }
    std::sort(merged,merged+count,[](const Message& a,const Message& b) { return a.id<b.id; });
    Message pending[kMaxSmsInFlight+4]; int pendingCount=0;
    for (int i=0;i<cv.count && pendingCount<kMaxSmsInFlight+4;++i)
        if (!cv.items[i].id && cv.items[i].delivery!=Delivery::Received) pending[pendingCount++]=cv.items[i];
    cv.count=0;
    const int skip=(std::max)(0,count+pendingCount-kMaxMessages);
    for (int i=skip;i<count;++i) cv.items[cv.count++]=merged[i];
    for (int i=0;i<pendingCount && cv.count<kMaxMessages;++i) cv.items[cv.count++]=pending[i];
    cv.pages = static_cast<uint8_t>(page + 1);
    cv.hasMore = (r.flags & proto::kFlagHasMore) && cv.count + proto::kSmsHistoryMaxPerPage <= kMaxMessages;
    cv.loading = false;
    if (cv.hasMore && cv.pages < kAutoHistoryPages) QueueHistory(cv.pages);
    Log("SmsHistory: page=%u count=%u total=%d more=%d", page, r.count, cv.count, cv.hasMore ? 1 : 0);
}

Message* FindPending(uint32_t reqId) {
    Conversation& cv = M().convo;
    for (int i = cv.count - 1; i >= 0; --i)
        if (cv.items[i].requestId == reqId) return &cv.items[i];
    return nullptr;
}

bool TakeSmsInFlight(uint32_t reqId) {
    for (auto& s : g_sms)
        if (s.reqId == reqId) { s = SmsInFlight(); return true; }
    return false;
}

void HandleSmsSendResult(const uint8_t* p, size_t n) {
    proto::SmsSendResultResponse r;
    if (!proto::DecodeSmsSendResult(p, n, r)) { Log("bad SmsSendResult payload"); return; }
    if (!TakeSmsInFlight(r.requestId)) { Log("stale SmsSendResult reqId=%u", r.requestId); return; }
    const bool ok = r.status == proto::SmsSendStatus::Stored;
    if (Message* msg = FindPending(r.requestId)) {
        auto& cv=M().convo;
        bool alreadyInHistory=false;
        if (ok && r.messageId)
            for (int i=0;i<cv.count;++i)
                if (&cv.items[i]!=msg && cv.items[i].id==r.messageId) alreadyInHistory=true;
        if (alreadyInHistory) {
            const int index=static_cast<int>(msg-cv.items);
            std::memmove(msg,msg+1,sizeof(Message)*(cv.count-index-1)); --cv.count;
        } else {
            msg->delivery = ok ? Delivery::Delivered : Delivery::Failed;
            msg->id = r.messageId;
            msg->requestId = 0;
        }
    }
    if (!ok) Notice("Message not sent", true);
    Log("SmsSendResult: reqId=%u status=%u msgId=%u", r.requestId, static_cast<unsigned>(r.status), r.messageId);
    QueueThreads(0);
}

void HandleSmsReceived(const uint8_t* p, size_t n) {
    proto::SmsReceivedPush r;
    if (!proto::DecodeSmsReceived(p, n, r)) { Log("bad SmsReceived payload"); return; }
    if (r.messageId) {
        for (const auto id : g_receivedIds) if (id==r.messageId) return;
        g_receivedIds[g_receivedPos++ % 128]=r.messageId;
    }
    Log("SmsReceived: msg=%u from=%.*s", r.messageId, static_cast<int>(r.senderNumberLen), r.senderNumber);

    Model& m = M();
    char sender[16];
    CopyN(sender, r.senderNumber, r.senderNumberLen);
    char name[24];
    CopyN(name, r.senderName, r.senderNameLen);
    char text[121];
    CopyN(text, r.text, r.textLen);

    // Conversation open with this sender: append (once) and treat as read.
    Conversation& cv = m.convo;
    const bool viewing = input::IsConversationVisible(sender);
    if (std::strcmp(cv.number, sender) == 0) {
        bool dup = false;
        for (int i = 0; i < cv.count; ++i) dup = dup || (r.messageId && cv.items[i].id == r.messageId);
        if (!dup) {
            if (cv.count == kMaxMessages) {
                std::memmove(&cv.items[0], &cv.items[1], sizeof(Message) * (kMaxMessages - 1));
                --cv.count;
            }
            Message& dst = cv.items[cv.count++];
            dst = Message();
            CopyText(dst.text, text);
            FormatTime(r.timestamp, dst.time);
            dst.id = r.messageId;
            dst.delivery = Delivery::Received;
        }
    }

    bool found = false;
    for (int i = 0; i < m.threadCount; ++i)
        if (std::strcmp(m.threads[i].number, sender) == 0) {
            found = true;
            if (!viewing && m.threads[i].unread < 99) ++m.threads[i].unread;
        }
    if (!found && m.threadCount < kMaxThreads) {
        // Shown at once; the refresh below replaces it with the server's ordering.
        std::memmove(&m.threads[1], &m.threads[0], sizeof(ThreadSummary) * m.threadCount);
        ThreadSummary& t = m.threads[0];
        t = ThreadSummary();
        CopyText(t.peer, name);
        CopyText(t.number, sender);
        CopyText(t.preview, text);
        FormatTime(r.timestamp, t.time);
        t.unread = viewing ? 0 : 1;
        t.hue = r.hue;
        ++m.threadCount;
    }

    if (!viewing && m.settings.banners) {
        if (input::IsVisible()) {
            input::OnSmsNotification(name, text, sender, r.hue, GetTickCount64());
        } else {
            char toast[160];
            snprintf(toast, sizeof(toast), "SMS from %s: %s", name, text);
            skr::native_api::ui::PushNotification(api::NotifyKind::Info, 6, toast);
        }
    }
    QueueThreads(0);
}

void HandleCallState(const uint8_t* p, size_t n) {
    proto::CallStateResponse r;
    if (!proto::DecodeCallState(p, n, r)) { Log("bad CallState payload"); return; }
    // Calls are not implemented by the Gate (M4: "calls not available"); a
    // CallState can only come from a newer server. Logged, never shown.
    Log("CallState ignored: session=%u state=%u", r.callSessionId, static_cast<unsigned>(r.state));
}

void HandlePhoneError(const uint8_t* p, size_t n) {
    proto::PhoneErrorResponse r;
    if (!proto::DecodePhoneError(p, n, r)) { Log("bad PhoneError payload"); return; }
    Log("PhoneError: reqId=%u code=%u msg=%s", r.requestId, static_cast<unsigned>(r.code), r.message);
    Model& m = M();
    const uint32_t id = r.requestId;
    if (!id) return;
    if (id == g_read.reqId) {
        g_read.Clear(); g_lastReadQueued=0; Notice(ErrorText(r.code),true);
    } else if (id == g_profile.reqId) {
        g_profile.Clear(); m.profilePending=false; Notice(ErrorText(r.code),true);
    } else if (id == g_sync.reqId) {
        g_sync.Clear();
        m.sync = SyncState::Error;
        CopyText(m.syncError, ErrorText(r.code));
    } else if (id == g_contacts.reqId) {
        g_contacts.Clear();
        m.contactsLoading = false;
        Notice(ErrorText(r.code), true);
    } else if (id == g_threads.reqId) {
        g_threads.Clear();
        m.threadsLoading = false;
        Notice(ErrorText(r.code), true);
    } else if (id == g_history.reqId) {
        g_history.Clear();
        m.convo.loading = false;
        Notice(ErrorText(r.code), true);
    } else if (id == g_contactOp.reqId) {
        g_contactOp.Clear();
        m.contactOpPending = false;
        m.contactOpResult = Model::OpResult::Failed;
        Notice(ErrorText(r.code), true);
    } else if (TakeSmsInFlight(id)) {
        if (Message* msg = FindPending(id)) {
            msg->delivery = Delivery::Failed;
            msg->requestId = 0;
        }
        Notice(ErrorText(r.code), true);
    }
    // Any other id: stale (already timed out or superseded) — ignored.
}

void ExpireTimeouts(uint64_t now) {
    Model& m = M();
    auto expired = [now](const Track& t) { return t.Busy() && now - t.at > kTimeoutMs; };
    if (expired(g_read)) { g_read.Clear(); g_lastReadQueued=0; }
    if (expired(g_profile)) { g_profile.Clear(); m.profilePending=false; Notice("Phone service not responding",true); }
    if (expired(g_sync)) {
        g_sync.Clear();
        m.sync = SyncState::Error;
        CopyText(m.syncError, "Phone service not responding");
    }
    if (expired(g_contacts)) { g_contacts.Clear(); m.contactsLoading = false; }
    if (expired(g_threads)) { g_threads.Clear(); m.threadsLoading = false; }
    if (expired(g_history)) { g_history.Clear(); m.convo.loading = false; }
    if (expired(g_contactOp)) {
        g_contactOp.Clear();
        m.contactOpPending = false;
        m.contactOpResult = Model::OpResult::Failed;
        Notice("Phone service not responding", true);
    }
    for (auto& s : g_sms)
        if (s.reqId && now - s.at > kTimeoutMs) {
            if (Message* msg = FindPending(s.reqId)) {
                msg->delivery = Delivery::Failed;
                msg->requestId = 0;
            }
            s = SmsInFlight();
            Notice("Message not sent", true);
        }
}

} // namespace

// ── Public API ──────────────────────────────────────────────────────

void OnPhoneReady(uint32_t sessionId, bool setupSupported, bool gatewayV1) {
    Reset();
    g_sessionId = sessionId;
    g_active    = true;
    g_transport=gatewayV1?protocol::skrx::phone::Transport::GatewayV1:protocol::skrx::phone::Transport::Legacy;
    g_logLines  = 0;
    Log("Phone ready, session=%08x", sessionId);
    M().setupSupported = setupSupported;
    M().sync = SyncState::Loading;
    RequestSync();
}

void Reset() {
    if (g_active) Log("Phone net reset");
    g_sessionId = 0;
    g_nextReqId = 1;
    g_active    = false;
    g_transport=protocol::skrx::phone::Transport::Disabled;
    g_qHead = g_qCount = 0;
    g_lastSendAt = 0;
    g_profile.Clear(); g_read.Clear(); g_lastReadQueued=0; g_readPeer[0]=0;
    std::memset(g_receivedIds,0,sizeof(g_receivedIds)); g_receivedPos=0;
    g_sync.Clear(); g_contacts.Clear(); g_threads.Clear(); g_history.Clear(); g_contactOp.Clear();
    for (auto& s : g_sms) s = SmsInFlight();
    g_historyPeer[0] = 0;

    // Nothing from the previous session may survive into the next one.
    Model& m = M();
    m.setupSupported=false; m.profileLoaded=false; m.profilePending=false;
    m.profile = proto::Profile(); m.settings = PhoneSettings();
    m.sync = SyncState::Offline;
    m.syncError[0] = 0;
    m.ownNumber[0] = 0;
    m.contactCount = 0;
    m.threadCount = 0;
    m.convo = Conversation();
    m.contactsLoading = m.threadsLoading = m.threadsHasMore = false;
    m.contactOpPending = false;
    m.contactOpResult = Model::OpResult::None;
    m.notice[0] = 0;
    m.noticeAt = 0;
}

void Tick(uint64_t now) {
    if (!g_active) return;
    if (g_qCount && now - g_lastSendAt >= kSendIntervalMs) {
        const Outgoing& o = g_queue[g_qHead];
        SendNow(o);                           // a failed send is caught by the timeout
        g_qHead = (g_qHead + 1) % kQueueSize;
        --g_qCount;
        g_lastSendAt = now;
    }
    ExpireTimeouts(now);
    MarkConversationRead();
}

static bool ApplyPhoneCall(uint8_t op, uint32_t callId, const uint8_t* payload, size_t payloadLen) {
    (void)callId;
    if (!g_active) return true;               // late op after reset: swallow
    switch (static_cast<api::Op>(op)) {
        case api::Op::PhoneReadResult: {
            api::Reader reader(payload,payloadLen); const auto id=reader.U32();
            if (reader.Done() && g_read.Busy() && id==g_read.reqId) g_read.Clear();
            return true;
        }
        case api::Op::PhoneProfile: {
            proto::Profile p;
            if (!M().setupSupported || !proto::DecodeProfile(payload,payloadLen,p) || p.requestId!=g_profile.reqId || !g_profile.Busy()) return true;
            g_profile.Clear();
            M().profile=p; M().profile.requestId=0; M().profileLoaded=true; M().profilePending=false;
            M().settings.uiScale=p.scale/100.0f; M().settings.banners=p.banners!=0;
            return true;
        }
        case api::Op::PhoneSync:     HandlePhoneSync(payload, payloadLen);     return true;
        case api::Op::ContactList:   HandleContactList(payload, payloadLen);   return true;
        case api::Op::ContactResult: HandleContactResult(payload, payloadLen); return true;
        case api::Op::SmsThreadList: HandleSmsThreadList(payload, payloadLen); return true;
        case api::Op::SmsHistory:    HandleSmsHistory(payload, payloadLen);    return true;
        case api::Op::SmsSendResult: HandleSmsSendResult(payload, payloadLen); return true;
        case api::Op::SmsReceived:   HandleSmsReceived(payload, payloadLen);   return true;
        case api::Op::CallState:     HandleCallState(payload, payloadLen);     return true;
        case api::Op::PhoneError:    HandlePhoneError(payload, payloadLen);    return true;
        default: return false;
    }
}

bool OnPhoneCall(uint8_t op,uint32_t callId,const uint8_t* data,size_t len) {
    if (g_active && g_transport!=protocol::skrx::phone::Transport::Legacy) return false;
    return ApplyPhoneCall(op,callId,data,len);
}
void OnPhoneGatewayMessage(const protocol::skrx::GatewayHeader& header,const uint8_t* data,size_t len) {
    if (!g_active || g_transport!=protocol::skrx::phone::Transport::GatewayV1) return;
    protocol::skrx::phone::Frame frame;
    if (!protocol::skrx::phone::Decode(header,data,len,false,M().setupSupported,frame) || frame.sessionId!=g_sessionId) return;
    ApplyPhoneCall(frame.op,0,frame.body,frame.bodyLen);
}

void MarkConversationRead() {
    if (!g_active || !M().setupSupported || g_read.Busy() || !input::IsConversationVisible(M().convo.number)) return;
    uint32_t last=0;
    for (int i=0;i<M().convo.count;++i) {
        const auto& msg=M().convo.items[i];
        if (msg.delivery==Delivery::Received && msg.id>last) last=msg.id;
    }
    if (!last || (last<=g_lastReadQueued && !std::strcmp(g_readPeer,M().convo.number))) return;
    uint8_t data[16]; api::Writer writer(data,sizeof(data));
    writer.Str8(M().convo.number,7); writer.U32(last);
    const auto id=Enqueue(proto::ClientOp::MarkRead,data,writer.size());
    if (!id) return;
    g_read={id,GetTickCount64(),0};
    g_lastReadQueued=last; CopyText(g_readPeer,M().convo.number);
    for (int i=0;i<M().threadCount;++i)
            if (!std::strcmp(M().threads[i].number,M().convo.number)) M().threads[i].unread=0;
}

bool RequestProfile() {
    if (!g_active || !M().setupSupported || g_profile.Busy() || M().sync!=SyncState::Ready) return false;
    const auto id=Enqueue(proto::ClientOp::ProfileGet,nullptr,0);
    if (!id) return false;
    g_profile={id,GetTickCount64(),0}; M().profilePending=true; return true;
}
bool SaveProfile(uint8_t stage, uint8_t wallpaper, uint8_t scale, bool banners) {
    if (!g_active || !M().setupSupported || !M().profileLoaded || g_profile.Busy()) return false;
    proto::Profile p=M().profile; p.requestId=0;
    p.stage=stage; p.wallpaper=wallpaper; p.scale=scale; p.banners=banners?1:0;
    uint8_t data[16]; const auto n=proto::EncodeProfile(p,data,sizeof(data));
    if (!n) return false;
    const auto id=Enqueue(proto::ClientOp::ProfileSave,data,n);
    if (!id) return false;
    g_profile={id,GetTickCount64(),0}; M().profilePending=true; return true;
}

bool RequestSync() {
    if (!g_active || g_sync.Busy()) return false;
    const uint32_t id = Enqueue(proto::ClientOp::PhoneSync, nullptr, 0);
    if (!id) return false;
    g_sync = {id, GetTickCount64(), 0};
    if (M().sync != SyncState::Ready) M().sync = SyncState::Loading;
    return true;
}

bool RefreshContacts() {
    if (!g_active || M().sync != SyncState::Ready || g_contacts.Busy()) return false;
    return QueueContacts(0);
}

bool RefreshThreads() {
    if (!g_active || M().sync != SyncState::Ready || g_threads.Busy()) return false;
    return QueueThreads(0);
}

bool OpenConversation(const char* peerNumber, const char* peerName, uint8_t hue) {
    char canonical[8]; if (!proto::NormalizePhoneNumber(peerNumber,canonical)) return false;
    peerNumber=canonical;
    if (!peerNumber || !proto::IsValidPhoneNumber(peerNumber, std::strlen(peerNumber))) return false;
    Model& m = M();
    Conversation& cv = m.convo;
    if (std::strcmp(cv.number, peerNumber) != 0) {
        cv = Conversation();
        CopyText(cv.number, peerNumber);
    }
    CopyText(cv.peer, peerName && *peerName ? peerName : peerNumber);
    cv.hue = hue;
    for (int i = 0; i < m.threadCount; ++i)
        if (std::strcmp(m.threads[i].number, peerNumber) == 0) m.threads[i].unread = 0;
    if (!g_active || m.sync != SyncState::Ready) return false;
    CopyText(g_historyPeer, peerNumber);
    cv.pages = 0;
    return QueueHistory(0);
}

bool LoadOlderHistory() {
    Conversation& cv = M().convo;
    if (!g_active || !cv.hasMore || cv.loading || g_history.Busy()) return false;
    if (std::strncmp(cv.number, g_historyPeer, proto::kPhoneNumberLen) != 0) return false;
    return QueueHistory(cv.pages);
}

bool AddContact(const char* number, const char* displayName) {
    char canonical[8]; if (!proto::NormalizePhoneNumber(number,canonical)) return false;
    number=canonical;
    Model& m = M();
    if (!g_active || m.sync != SyncState::Ready || g_contactOp.Busy() || !number) return false;
    proto::ContactAddRequest req;
    req.numberLen = static_cast<uint8_t>((std::min)(std::strlen(number), proto::kPhoneNumberLen + 1));
    if (!proto::IsValidPhoneNumber(number, req.numberLen)) return false;
    std::memcpy(req.number, number, req.numberLen);
    // The server lists only named contacts; an empty name falls back to the number.
    const char* name = displayName && *displayName ? displayName : number;
    req.nameLen = static_cast<uint8_t>((std::min)(std::strlen(name), proto::kMaxContactName));
    std::memcpy(req.name, name, req.nameLen);
    uint8_t buf[64];
    const size_t n = proto::EncodeContactAdd(req, buf, sizeof(buf));
    const uint32_t id = n ? Enqueue(proto::ClientOp::ContactAdd, buf, n) : 0;
    if (!id) return false;
    g_contactOp = {id, GetTickCount64(), 0};
    m.contactOpPending = true;
    m.contactOpResult = Model::OpResult::None;
    return true;
}

bool RemoveContact(const char* number) {
    char canonical[8]; if (!proto::NormalizePhoneNumber(number,canonical)) return false;
    number=canonical;
    Model& m = M();
    if (!g_active || m.sync != SyncState::Ready || g_contactOp.Busy() || !number) return false;
    proto::ContactRemoveRequest req;
    req.numberLen = static_cast<uint8_t>((std::min)(std::strlen(number), proto::kPhoneNumberLen + 1));
    if (!proto::IsValidPhoneNumber(number, req.numberLen)) return false;
    std::memcpy(req.number, number, req.numberLen);
    uint8_t buf[16];
    const size_t n = proto::EncodeContactRemove(req, buf, sizeof(buf));
    const uint32_t id = n ? Enqueue(proto::ClientOp::ContactRemove, buf, n) : 0;
    if (!id) return false;
    g_contactOp = {id, GetTickCount64(), 0};
    m.contactOpPending = true;
    m.contactOpResult = Model::OpResult::None;
    return true;
}

bool SendSms(const char* recipientNumber, const char* text) {
    char canonical[8]; if (!proto::NormalizePhoneNumber(recipientNumber,canonical)) return false;
    recipientNumber=canonical;
    Model& m = M();
    if (!g_active || m.sync != SyncState::Ready || !recipientNumber || !text) return false;
    const size_t numLen = std::strlen(recipientNumber), textLen = std::strlen(text);
    if (!proto::IsValidPhoneNumber(recipientNumber, numLen) || !textLen || textLen > proto::kMaxSmsText) return false;
    SmsInFlight* slot = nullptr;
    for (auto& s : g_sms)
        if (!s.reqId) { slot = &s; break; }
    if (!slot) { Notice("Wait for the previous messages", true); return false; }

    proto::SmsSendRequest req;
    req.recipientNumberLen = static_cast<uint8_t>(numLen);
    std::memcpy(req.recipientNumber, recipientNumber, numLen);
    req.textLen = static_cast<uint8_t>(textLen);
    std::memcpy(req.text, text, textLen);
    uint8_t buf[api::kMaxClientRequestPayload];
    const size_t n = proto::EncodeSmsSend(req, buf, sizeof(buf));
    const uint32_t id = n ? Enqueue(proto::ClientOp::SmsSend, buf, n) : 0;
    if (!id) return false;
    *slot = {id, GetTickCount64()};

    // Shown at once as Sending; only SmsSendResult may mark it sent.
    Conversation& cv = m.convo;
    if (std::strcmp(cv.number, recipientNumber) == 0) {
        if (cv.count == kMaxMessages) {
            std::memmove(&cv.items[0], &cv.items[1], sizeof(Message) * (kMaxMessages - 1));
            --cv.count;
        }
        Message& msg = cv.items[cv.count++];
        msg = Message();
        CopyText(msg.text, text);
        FormatTime(static_cast<uint32_t>(time(nullptr)), msg.time);
        msg.delivery = Delivery::Sending;
        msg.requestId = id;
    }
    return true;
}

bool IsActive() { return g_active; }

} // namespace skr::phone::net
