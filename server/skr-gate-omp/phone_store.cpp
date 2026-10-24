// M5.8: existing phone SQL/codecs isolated from all SDK/player/network calls.
#include "phone_worker.h"
#include "phone_m5_sql.h"
#include "protocol/phone_profile.h"
#include "protocol/skrx_phone.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
namespace skr::phone_backend {
using namespace protocol;
using namespace protocol::api;
using namespace protocol::phone;
using StringView=std::string_view;
static constexpr int kMaxContactsPerOwner=64;
static std::string SqlEscape(const char* s,size_t n) {
    std::string out; for(size_t i=0;i<n;++i) { if(s[i]=='\'') out+="''"; else out+=s[i]; } return out;
}
static uint32_t UnixNow() { return static_cast<uint32_t>(std::time(nullptr)); }
static uint8_t NumberHue(const char* s,size_t n) { uint32_t h=2166136261u; for(size_t i=0;i<n;++i) h=(h^static_cast<uint8_t>(s[i]))*16777619u; return static_cast<uint8_t>(h%8); }
bool Store::Initialize(const std::string& path) {
    if (!database_.Open(path)) return false;
    const char* ddl[] = {
        "CREATE TABLE IF NOT EXISTS phone_meta (key TEXT PRIMARY KEY, value TEXT NOT NULL)",

        "CREATE TABLE IF NOT EXISTS phone_numbers (nick TEXT PRIMARY KEY, number TEXT NOT NULL UNIQUE, created INTEGER NOT NULL)",
        "CREATE UNIQUE INDEX IF NOT EXISTS idx_phone_number ON phone_numbers(number)",
        "CREATE TABLE IF NOT EXISTS phone_contacts (id INTEGER PRIMARY KEY AUTOINCREMENT, owner TEXT NOT NULL, number TEXT NOT NULL, name TEXT NOT NULL, created INTEGER NOT NULL, UNIQUE(owner, number))",
        "CREATE INDEX IF NOT EXISTS idx_contacts_owner ON phone_contacts(owner)",
        "CREATE TABLE IF NOT EXISTS phone_messages (id INTEGER PRIMARY KEY AUTOINCREMENT, sender TEXT NOT NULL, recipient TEXT NOT NULL, text TEXT NOT NULL, ts INTEGER NOT NULL)",
        "CREATE INDEX IF NOT EXISTS idx_messages_pair ON phone_messages(sender, recipient, ts DESC)",
        "CREATE INDEX IF NOT EXISTS idx_messages_pair_rev ON phone_messages(recipient, sender, ts DESC)",
        skr::phone_m5::kReceiptSql,
        skr::phone_m5::kReadsSql,
    };

    auto execute = [&](const char* sql) {
        auto* result = phoneDb_->executeQuery(StringView(sql));
        if (!result) return false;
        delete result; return true;
    };
    if (!execute("PRAGMA busy_timeout=100") || !execute("BEGIN IMMEDIATE")) { database_.Close(); return false; }
    for (const char* sql : ddl) {
        if (!execute(sql)) { execute("ROLLBACK"); database_.Close(); return false; }
    }
    for (const char* sql : {skr::phone_m5::kPoolSql, skr::phone_m5::kFillPoolSql,
                           skr::phone_m5::kProfileSql, skr::phone_m5::kLegacyProfilesSql}) {
        if (!execute(sql)) { execute("ROLLBACK"); database_.Close(); return false; }
    }
    if (!execute("COMMIT")) { execute("ROLLBACK"); database_.Close(); return false; }
    return true;
}
std::vector<Response> Store::Process(const Request& work) {
    std::vector<Response> output;
    database_.SetLease(work.identity.lease);
    struct Cleanup { Database& db; ~Cleanup() { db.FinishRequest(); } } cleanup{database_};
    ClientRequest req; req.op=work.op; req.requestId=work.requestId; req.payload=work.data; req.payloadLen=static_cast<uint8_t>(work.size);
    const char* authName=work.identity.owner; const uint32_t sessionId=work.identity.sessionId;
    const bool hasSetup=work.identity.setup;
    const uint8_t historyPageSize=work.identity.gateway?protocol::skrx::phone::kHistoryPageSize:kSmsHistoryMaxPerPage;
    auto send=[&](Op op,const uint8_t* data,size_t size) {
        if (!size || size>sizeof(Response::data)) return;
        Response event; event.identity=work.identity; event.op=op; event.size=size;
        std::memcpy(event.data,data,size); output.push_back(std::move(event));
    };
    auto sendError = [&](uint32_t requestId, ErrorCode code, const char* msg = "") {
        PhoneErrorResponse err;
        err.requestId = requestId;
        err.code = code;
        err.messageLen = static_cast<uint8_t>(std::strlen(msg));
        if (err.messageLen > kMaxErrorMessage) err.messageLen = kMaxErrorMessage;
        std::memcpy(err.message, msg, err.messageLen);
        err.message[err.messageLen] = '\0';
        uint8_t buf[128];
        const size_t n = EncodePhoneError(err, buf, sizeof(buf));
        if (n) send(Op::PhoneError, buf, n);
    };

    if (!work.identity.lease || !work.identity.lease->load()) {
        sendError(req.requestId, ErrorCode::NoPhone, "not authenticated");
        return output;
    }

    if (!phoneDb_) {
        sendError(req.requestId, ErrorCode::InternalError, "phone db not ready");
        return output;
    }

    if (!IsKnownClientOp(req.op)) {
        sendError(req.requestId,ErrorCode::InternalError,"unsupported phone op");
        return output;
    }

    const auto op = static_cast<ClientOp>(req.op);
    const uint8_t* payload = req.payload;
    const size_t payloadLen = req.payloadLen;
    const std::string nick = SqlEscape(authName, std::strlen(authName));

    // Helper: look up this player's phone number from the DB
    auto lookupNumber = [&](char* outNumber) -> bool {
        char sql[128];
        std::snprintf(sql, sizeof(sql),
            "SELECT number FROM phone_numbers WHERE nick='%s'", nick.c_str());
        Result* rs = phoneDb_->executeQuery(StringView(sql));
        if (!rs) return false;
        bool found = false;
        if (rs->getRowCount() > 0) {
            StringView num = rs->getFieldString(0);
            if (num.length() == kPhoneNumberLen) {
                std::memcpy(outNumber, num.data(), kPhoneNumberLen);
                outNumber[kPhoneNumberLen] = '\0';
                found = true;
            }
        }
        delete rs;
        return found;
    };

    if ((op == ClientOp::ProfileGet || op == ClientOp::ProfileSave || op == ClientOp::MarkRead) && !hasSetup) {
        sendError(req.requestId, ErrorCode::NoPhone, "setup capability not negotiated"); return output;
    }
    switch (op) {
        case ClientOp::MarkRead: {
            Reader reader(payload,payloadLen); char peer[8]; uint8_t peerLen=0;
            if (!reader.Text8(peer,7,peerLen)) { sendError(req.requestId,ErrorCode::InvalidNumber); return output; }
            const auto last=reader.U32();
            if (!reader.Done() || !IsValidPhoneNumber(peer,peerLen)) { sendError(req.requestId,ErrorCode::InvalidNumber); return output; }
            char own[8]; if (!lookupNumber(own)) { sendError(req.requestId,ErrorCode::NoPhone); return output; }
            char sql[512];
            std::snprintf(sql,sizeof(sql),
                skr::phone_m5::kMarkReadSql,
                own,peer,own,peer,own,peer,last);
            auto* rs=phoneDb_->executeQuery(StringView(sql));
            if (rs) {
                delete rs;
                uint8_t ack[4]; Writer writer(ack,sizeof(ack)); writer.U32(req.requestId);
                send(Op::PhoneReadResult,ack,writer.size());
            } else sendError(req.requestId,ErrorCode::InternalError,"read state not saved");
            break;
        }
        case ClientOp::ProfileGet:
        case ClientOp::ProfileSave: {
            Profile profile;
            if (op == ClientOp::ProfileGet && payloadLen != 0) {
                sendError(req.requestId, ErrorCode::InternalError, "bad profile request"); return output;
            }
            if (op == ClientOp::ProfileSave && (!DecodeProfile(payload,payloadLen,profile) || profile.requestId != 0)) {
                sendError(req.requestId, ErrorCode::InternalError, "bad profile"); return output;
            }
            char sql[512];
            if (op == ClientOp::ProfileSave) {
                // No client identity, no activation reset, one acknowledged stage at a time.
                std::snprintf(sql,sizeof(sql),
                    skr::phone_m5::kSaveProfileSql,
                    profile.stage,profile.language,profile.wallpaper,profile.scale,profile.banners,
                    nick.c_str(),profile.stage,profile.stage);
                auto* rs=phoneDb_->executeQuery(StringView(sql));
                if (!rs) { sendError(req.requestId,ErrorCode::InternalError,"profile save failed"); return output; }
                delete rs;
            }
            std::snprintf(sql,sizeof(sql),"SELECT stage,language,wallpaper,scale,banners FROM phone_profiles WHERE nick='%s'",nick.c_str());
            auto* rs=phoneDb_->executeQuery(StringView(sql));
            if (!rs) { sendError(req.requestId,ErrorCode::InternalError,"profile unavailable"); return output; }
            if (!rs->getRowCount()) { delete rs; sendError(req.requestId,ErrorCode::NoPhone,"sync phone first"); return output; }
            profile.requestId=req.requestId;
            profile.stage=static_cast<uint8_t>(rs->getFieldInt(0));
            profile.language=static_cast<uint8_t>(rs->getFieldInt(1));
            profile.wallpaper=static_cast<uint8_t>(rs->getFieldInt(2));
            profile.scale=static_cast<uint8_t>(rs->getFieldInt(3));
            profile.banners=static_cast<uint8_t>(rs->getFieldInt(4));
            delete rs;
            uint8_t buf[16]; const auto n=EncodeProfile(profile,buf,sizeof(buf));
            if (n) send(Op::PhoneProfile,buf,n);
            break;
        }
        case ClientOp::PhoneSync: {
            if (payloadLen) { sendError(req.requestId,ErrorCode::InternalError,"sync payload must be empty"); return output; }
            char ownNumber[kPhoneNumberLen + 1] = {};
            if (!LookupOrAllocate(authName, ownNumber, true)) {
                sendError(req.requestId, ErrorCode::InternalError, "number allocation failed");
                return output;
            }

            PhoneSyncResponse resp;
            resp.requestId = req.requestId;
            std::memcpy(resp.ownNumber, ownNumber, kPhoneNumberLen);
            resp.ownNumberLen = kPhoneNumberLen;
            resp.flags = 0;
            uint8_t buf[64];
            const size_t n = EncodePhoneSync(resp, buf, sizeof(buf));
            if (n) send(Op::PhoneSync, buf, n);
            break;
        }

        case ClientOp::ContactList: {
            ContactListRequest clReq;
            if (!DecodeContactListRequest(payload, payloadLen, clReq)) {
                sendError(req.requestId, ErrorCode::InternalError, "bad payload");
                return output;
            }

            char ownNumber[kPhoneNumberLen + 1] = {};
            if (!lookupNumber(ownNumber)) {
                sendError(req.requestId, ErrorCode::NoPhone, "no phone number");
                return output;
            }
            const std::string numEsc = SqlEscape(ownNumber, kPhoneNumberLen);
            const int offset = static_cast<int>(clReq.page) * kContactListMaxPerPage;

            char sql[256];
            std::snprintf(sql, sizeof(sql),
                "SELECT number, name FROM phone_contacts WHERE owner='%s' ORDER BY name LIMIT %d OFFSET %d",
                numEsc.c_str(), kContactListMaxPerPage + 1, offset);
            Result* rs = phoneDb_->executeQuery(StringView(sql));
            if (!rs) { sendError(req.requestId, ErrorCode::InternalError, "db error"); return output; }

            ContactListResponse resp;
            resp.requestId = req.requestId;
            resp.count = 0;
            resp.flags = 0;
            const size_t rowCount = rs->getRowCount();
            bool hasMore = rowCount > kContactListMaxPerPage;

            for (size_t i = 0; i < rowCount && resp.count < kContactListMaxPerPage; ++i) {
                if (i > 0 && !rs->selectNextRow()) break;
                auto& e = resp.entries[resp.count];
                StringView num = rs->getFieldString(0);
                StringView name = rs->getFieldString(1);
                if (num.length() == kPhoneNumberLen && name.length() <= kMaxContactName && name.length() > 0) {
                    std::memcpy(e.number, num.data(), num.length());
                    e.numberLen = static_cast<uint8_t>(num.length());
                    std::memcpy(e.name, name.data(), name.length());
                    e.nameLen = static_cast<uint8_t>(name.length());
                    e.hue = NumberHue(e.number, e.numberLen);
                    ++resp.count;
                }
            }
            delete rs;

            if (hasMore) resp.flags |= kFlagHasMore;
            uint8_t buf[400];
            const size_t n = EncodeContactList(resp, buf, sizeof(buf));
            if (n) send(Op::ContactList, buf, n);
            break;
        }

        case ClientOp::ContactAdd: {
            ContactAddRequest caReq;
            if (!DecodeContactAdd(payload, payloadLen, caReq)) {
                sendError(req.requestId, ErrorCode::InternalError, "bad payload");
                return output;
            }

            char ownNumber[kPhoneNumberLen + 1] = {};
            if (!lookupNumber(ownNumber)) {
                sendError(req.requestId, ErrorCode::NoPhone, "no phone number");
                return output;
            }
            const std::string numEsc = SqlEscape(ownNumber, kPhoneNumberLen);
            const std::string contactNum = SqlEscape(caReq.number, caReq.numberLen);
            const std::string contactName = SqlEscape(caReq.name, caReq.nameLen);

            // Self-add check
            if (std::strncmp(ownNumber, caReq.number, kPhoneNumberLen) == 0) {
                sendError(req.requestId, ErrorCode::SelfAction, "cannot add self");
                return output;
            }

            // Check contact limit
            char sql[256];
            std::snprintf(sql, sizeof(sql),
                "SELECT COUNT(*) FROM phone_contacts WHERE owner='%s'", numEsc.c_str());
            Result* rs = phoneDb_->executeQuery(StringView(sql));
            if (rs) {
                long count = rs->getFieldInt(0);
                delete rs;
                if (count >= kMaxContactsPerOwner) {
                    sendError(req.requestId, ErrorCode::ContactsFull, "max contacts reached");
                    return output;
                }
            }

            // Check if the number exists in the system
            std::snprintf(sql, sizeof(sql),
                "SELECT nick FROM phone_numbers WHERE number='%s'", contactNum.c_str());
            rs = phoneDb_->executeQuery(StringView(sql));
            bool numberExists = rs && rs->getRowCount() > 0;
            if (rs) delete rs;
            if (!numberExists) {
                sendError(req.requestId, ErrorCode::InvalidNumber, "number not found");
                return output;
            }

            std::snprintf(sql, sizeof(sql),
                "INSERT OR IGNORE INTO phone_contacts(owner, number, name, created) VALUES('%s', '%s', '%s', %u)",
                numEsc.c_str(), contactNum.c_str(), contactName.c_str(), UnixNow());
            rs = phoneDb_->executeQuery(StringView(sql));
            if (!rs) { sendError(req.requestId,ErrorCode::InternalError,"contact not saved"); return output; }
            delete rs;

            ContactResultResponse resp;
            resp.requestId = req.requestId;
            resp.status = ContactResultStatus::Ok;
            uint8_t buf[16];
            const size_t n = EncodeContactResult(resp, buf, sizeof(buf));
            if (n) send(Op::ContactResult, buf, n);
            break;
        }

        case ClientOp::ContactRemove: {
            ContactRemoveRequest crReq;
            if (!DecodeContactRemove(payload, payloadLen, crReq)) {
                sendError(req.requestId, ErrorCode::InternalError, "bad payload");
                return output;
            }

            char ownNumber[kPhoneNumberLen + 1] = {};
            if (!lookupNumber(ownNumber)) {
                sendError(req.requestId, ErrorCode::NoPhone, "no phone number");
                return output;
            }
            const std::string numEsc = SqlEscape(ownNumber, kPhoneNumberLen);
            const std::string contactNum = SqlEscape(crReq.number, crReq.numberLen);

            char sql[256];
            std::snprintf(sql, sizeof(sql),
                "DELETE FROM phone_contacts WHERE owner='%s' AND number='%s'",
                numEsc.c_str(), contactNum.c_str());
            Result* rs = phoneDb_->executeQuery(StringView(sql));
            if (!rs) { sendError(req.requestId,ErrorCode::InternalError,"contact not removed"); return output; }
            delete rs;

            ContactResultResponse resp;
            resp.requestId = req.requestId;
            resp.status = ContactResultStatus::Ok;
            uint8_t buf[16];
            const size_t n = EncodeContactResult(resp, buf, sizeof(buf));
            if (n) send(Op::ContactResult, buf, n);
            break;
        }

        case ClientOp::SmsThreads: {
            SmsThreadsRequest stReq;
            if (!DecodeSmsThreadsRequest(payload, payloadLen, stReq)) {
                sendError(req.requestId, ErrorCode::InternalError, "bad payload");
                return output;
            }

            char ownNumber[kPhoneNumberLen + 1] = {};
            if (!lookupNumber(ownNumber)) {
                sendError(req.requestId, ErrorCode::NoPhone, "no phone number");
                return output;
            }
            const std::string numEsc = SqlEscape(ownNumber, kPhoneNumberLen);
            const int offset = static_cast<int>(stReq.page) * kSmsThreadMaxPerPage;

            // Thread summary query: group messages by peer, get latest timestamp and preview
            char sql[1024];
            std::snprintf(sql, sizeof(sql),
                "SELECT peer, "
                "(SELECT text FROM phone_messages m2 "
                "WHERE (m2.sender='%s' AND m2.recipient=peer) "
                "OR (m2.recipient='%s' AND m2.sender=peer) "
                "ORDER BY m2.id DESC LIMIT 1) AS preview, "
                "latest_ts FROM ("
                "SELECT CASE WHEN sender='%s' THEN recipient ELSE sender END AS peer, "
                "MAX(ts) AS latest_ts, MAX(id) AS latest_id FROM phone_messages "
                "WHERE sender='%s' OR recipient='%s' "
                "GROUP BY peer) threads "
                "ORDER BY latest_id DESC LIMIT %d OFFSET %d",
                numEsc.c_str(), numEsc.c_str(),
                numEsc.c_str(), numEsc.c_str(), numEsc.c_str(),
                kSmsThreadMaxPerPage + 1, offset);

            Result* rs = phoneDb_->executeQuery(StringView(sql));
            if (!rs) { sendError(req.requestId, ErrorCode::InternalError, "db error"); return output; }

            SmsThreadListResponse resp;
            resp.requestId = req.requestId;
            resp.count = 0;
            resp.flags = 0;
            const size_t rowCount = rs->getRowCount();
            bool hasMore = rowCount > kSmsThreadMaxPerPage;

            for (size_t i = 0; i < rowCount && resp.count < kSmsThreadMaxPerPage; ++i) {
                if (i > 0 && !rs->selectNextRow()) break;
                auto& e = resp.entries[resp.count];

                StringView peerNum = rs->getFieldString(0);
                StringView preview = rs->getFieldString(1);
                long ts = rs->getFieldInt(2);

                if (peerNum.length() != kPhoneNumberLen) continue;
                std::memcpy(e.peerNumber, peerNum.data(), kPhoneNumberLen);
                e.peerNumberLen = kPhoneNumberLen;

                // Resolve peer name from contacts
                char nameSql[256];
                std::snprintf(nameSql, sizeof(nameSql),
                    "SELECT name FROM phone_contacts WHERE owner='%s' AND number='%.*s'",
                    numEsc.c_str(), static_cast<int>(peerNum.length()), peerNum.data());
                Result* nameRs = phoneDb_->executeQuery(StringView(nameSql));
                if (nameRs && nameRs->getRowCount() > 0) {
                    StringView name = nameRs->getFieldString(0);
                    size_t nameLen = name.length() <= kMaxPeerName ? name.length() : kMaxPeerName;
                    std::memcpy(e.peerName, name.data(), nameLen);
                    e.peerNameLen = static_cast<uint8_t>(nameLen);
                } else {
                    std::memcpy(e.peerName, peerNum.data(), kPhoneNumberLen);
                    e.peerNameLen = kPhoneNumberLen;
                }
                if (nameRs) delete nameRs;

                size_t prevLen = preview.length() <= kMaxPreviewText ? preview.length() : kMaxPreviewText;
                std::memcpy(e.preview, preview.data(), prevLen);
                e.previewLen = static_cast<uint8_t>(prevLen);
                e.timestamp = static_cast<uint32_t>(ts);
                char unreadSql[512];
                std::snprintf(unreadSql,sizeof(unreadSql),
                    "SELECT COUNT(*) FROM phone_messages WHERE recipient='%s' AND sender='%.*s' AND id>COALESCE((SELECT last_id FROM phone_reads WHERE owner='%s' AND peer='%.*s'),0)",
                    numEsc.c_str(),static_cast<int>(peerNum.length()),peerNum.data(),numEsc.c_str(),static_cast<int>(peerNum.length()),peerNum.data());
                auto* unreadRs=phoneDb_->executeQuery(StringView(unreadSql));
                const long unread=unreadRs && unreadRs->getRowCount()?unreadRs->getFieldInt(0):0;
                if (unreadRs) delete unreadRs;
                e.unread=static_cast<uint8_t>(unread>99?99:unread);
                e.hue = NumberHue(e.peerNumber, e.peerNumberLen);
                ++resp.count;
            }
            delete rs;

            if (hasMore) resp.flags |= kFlagHasMore;
            uint8_t buf[400];
            const size_t n = EncodeSmsThreadList(resp, buf, sizeof(buf));
            if (n) send(Op::SmsThreadList, buf, n);
            break;
        }

        case ClientOp::SmsHistory: {
            SmsHistoryRequest shReq;
            if (!DecodeSmsHistoryReq(payload, payloadLen, shReq)) {
                sendError(req.requestId, ErrorCode::InternalError, "bad payload");
                return output;
            }

            char ownNumber[kPhoneNumberLen + 1] = {};
            if (!lookupNumber(ownNumber)) {
                sendError(req.requestId, ErrorCode::NoPhone, "no phone number");
                return output;
            }
            const std::string numEsc = SqlEscape(ownNumber, kPhoneNumberLen);
            const std::string peerEsc = SqlEscape(shReq.peerNumber, shReq.peerNumberLen);
            const int offset = static_cast<int>(shReq.page) * historyPageSize;

            char sql[512];
            std::snprintf(sql, sizeof(sql),
                "SELECT id, sender, text, ts FROM phone_messages "
                "WHERE (sender='%s' AND recipient='%s') OR (sender='%s' AND recipient='%s') "
                "ORDER BY id DESC LIMIT %d OFFSET %d",
                numEsc.c_str(), peerEsc.c_str(), peerEsc.c_str(), numEsc.c_str(),
                historyPageSize + 1, offset);

            Result* rs = phoneDb_->executeQuery(StringView(sql));
            if (!rs) { sendError(req.requestId, ErrorCode::InternalError, "db error"); return output; }

            SmsHistoryResponse resp;
            resp.requestId = req.requestId;
            resp.count = 0;
            resp.flags = 0;
            const size_t rowCount = rs->getRowCount();
            bool hasMore = rowCount > historyPageSize;

            for (size_t i = 0; i < rowCount && resp.count < historyPageSize; ++i) {
                if (i > 0 && !rs->selectNextRow()) break;
                auto& e = resp.entries[resp.count];

                e.messageId = static_cast<uint32_t>(rs->getFieldInt(0));
                StringView sender = rs->getFieldString(1);
                StringView text = rs->getFieldString(2);
                e.timestamp = static_cast<uint32_t>(rs->getFieldInt(3));

                size_t textLen = text.length() <= kMaxSmsText ? text.length() : kMaxSmsText;
                std::memcpy(e.text, text.data(), textLen);
                e.textLen = static_cast<uint8_t>(textLen);
                e.delivery = 0;
                e.direction = (sender.length() == kPhoneNumberLen &&
                              std::memcmp(sender.data(), ownNumber, kPhoneNumberLen) == 0)
                    ? SmsDirection::Outgoing : SmsDirection::Incoming;
                ++resp.count;
            }
            delete rs;

            if (hasMore) resp.flags |= kFlagHasMore;
            uint8_t buf[400];
            const size_t n = EncodeSmsHistory(resp, buf, sizeof(buf));
            if (n) send(Op::SmsHistory, buf, n);
            break;
        }

        case ClientOp::SmsSend: {
            SmsSendRequest ssReq;
            if (!DecodeSmsSend(payload, payloadLen, ssReq)) {
                sendError(req.requestId, ErrorCode::InternalError, "bad payload");
                return output;
            }

            char ownNumber[kPhoneNumberLen + 1] = {};
            if (!lookupNumber(ownNumber)) {
                sendError(req.requestId, ErrorCode::NoPhone, "no phone number");
                return output;
            }

            // Self-send check
            if (std::strncmp(ownNumber, ssReq.recipientNumber, kPhoneNumberLen) == 0) {
                sendError(req.requestId, ErrorCode::SelfAction, "cannot message self");
                return output;
            }

            // Verify recipient exists
            const std::string recipEsc = SqlEscape(ssReq.recipientNumber, ssReq.recipientNumberLen);
            char sql[256];
            std::snprintf(sql, sizeof(sql),
                "SELECT nick FROM phone_numbers WHERE number='%s'", recipEsc.c_str());
            Result* rs = phoneDb_->executeQuery(StringView(sql));
            bool recipExists = rs && rs->getRowCount() > 0;
            if (rs) delete rs;
            if (!recipExists) {
                sendError(req.requestId, ErrorCode::InvalidNumber, "recipient not found");
                return output;
            }

            // Durable idempotency for a retransmitted request in the same API session.
            // M4 has no reconnect-stable client token: a new session is a new send.
            auto execute = [&](const char* query) {
                auto* result=phoneDb_->executeQuery(StringView(query));
                if (!result) return false;
                delete result; return true;
            };
            if (!execute("BEGIN IMMEDIATE")) { sendError(req.requestId,ErrorCode::InternalError,"phone database busy"); return output; }
            char receiptSql[512];
            std::snprintf(receiptSql,sizeof(receiptSql),
                "SELECT message_id FROM phone_sms_receipts WHERE owner='%s' AND session=%u AND request=%u",
                nick.c_str(),sessionId,req.requestId);
            rs=phoneDb_->executeQuery(StringView(receiptSql));
            if (!rs) { execute("ROLLBACK"); sendError(req.requestId,ErrorCode::InternalError,"receipt lookup failed"); return output; }
            uint32_t messageId=rs->getRowCount()?static_cast<uint32_t>(rs->getFieldInt(0)):0;
            delete rs;
            const bool duplicate=messageId!=0;
            const uint32_t now=UnixNow();
            if (!duplicate) {
                const auto numEsc=SqlEscape(ownNumber,kPhoneNumberLen);
                const auto textEsc=SqlEscape(ssReq.text,ssReq.textLen);
                char insertSql[512];
                std::snprintf(insertSql,sizeof(insertSql),
                    "INSERT INTO phone_messages(sender, recipient, text, ts) VALUES('%s','%s','%s',%u)",
                    numEsc.c_str(),recipEsc.c_str(),textEsc.c_str(),now);
                if (!execute(insertSql)) { execute("ROLLBACK"); sendError(req.requestId,ErrorCode::InternalError,"message not stored"); return output; }
                rs=phoneDb_->executeQuery(StringView("SELECT last_insert_rowid()"));
                if (rs) {
                    if (rs->getRowCount()) messageId=static_cast<uint32_t>(rs->getFieldInt(0));
                    delete rs;
                }
                std::snprintf(receiptSql,sizeof(receiptSql),
                    "INSERT INTO phone_sms_receipts(owner,session,request,message_id) VALUES('%s',%u,%u,%u)",
                    nick.c_str(),sessionId,req.requestId,messageId);
                if (!messageId || !execute(receiptSql)) { execute("ROLLBACK"); sendError(req.requestId,ErrorCode::InternalError,"message not stored"); return output; }
            }
            if (!execute("COMMIT")) { execute("ROLLBACK"); sendError(req.requestId,ErrorCode::InternalError,"message commit failed"); return output; }

            // Preserve persistent conversation history; no implicit destructive pruning.

            SmsSendResultResponse resp;
            resp.requestId = req.requestId;
            resp.status = SmsSendStatus::Stored;
            resp.messageId = messageId;
            uint8_t buf[16];
            const size_t n = EncodeSmsSendResult(resp, buf, sizeof(buf));
            if (n) send(Op::SmsSendResult, buf, n);

            if (!duplicate) PushSms(ownNumber, ssReq.recipientNumber, ssReq.text, ssReq.textLen, now, messageId, output);
            break;
        }

        case ClientOp::CallDial:
        case ClientOp::CallAccept:
        case ClientOp::CallDecline:
        case ClientOp::CallHangup:
            sendError(req.requestId, ErrorCode::InternalError, "calls not available");
            break;
    }
    return output;
}
bool Store::LookupOrAllocate(const char* nickRaw, char* outNumber, bool allowAllocate) {
    if (!phoneDb_ || !nickRaw || !nickRaw[0]) return false;
    const std::string nick = SqlEscape(nickRaw, std::strlen(nickRaw));

    char sql[256];
    std::snprintf(sql, sizeof(sql), "SELECT number FROM phone_numbers WHERE nick='%s'", nick.c_str());
    Result* rs = phoneDb_->executeQuery(StringView(sql));
    if (!rs) return false;
    bool found = false;
    if (rs->getRowCount() > 0) {
        StringView num = rs->getFieldString(0);
        if (num.length() == kPhoneNumberLen) {
            std::memcpy(outNumber, num.data(), kPhoneNumberLen);
            outNumber[kPhoneNumberLen] = '\0';
            found = true;
        }
    }
    delete rs;
    if (found || !allowAllocate) return found;

    auto execute = [&](const char* query) {
        auto* result = phoneDb_->executeQuery(StringView(query));
        if (!result) return false;
        delete result; return true;
    };
    if (!execute("BEGIN IMMEDIATE")) return false;
    if (LookupOrAllocate(nickRaw, outNumber, false)) {
        if (execute("COMMIT")) return true;
        execute("ROLLBACK"); return false;
    }
    char allocation[512];
    std::snprintf(allocation, sizeof(allocation), skr::phone_m5::kAllocateSql,
                  nick.c_str(), UnixNow());
    if (!execute(allocation) || !LookupOrAllocate(nickRaw, outNumber, false)) {
        execute("ROLLBACK");
        return false;
    }
    std::snprintf(allocation, sizeof(allocation),
        "INSERT INTO phone_profiles(nick) VALUES('%s')", nick.c_str());
    if (!execute(allocation) || !execute("COMMIT")) {
        execute("ROLLBACK"); outNumber[0]=0; return false;
    }
    return true;
}

void Store::PushSms(const char* senderNumber, const char* recipientNumber,
                                    const char* text, size_t textLen, uint32_t ts, uint32_t messageId, std::vector<Response>& output) {
    if (!phoneDb_ || !textLen) return;
    const std::string recipEsc = SqlEscape(recipientNumber, kPhoneNumberLen);
    const std::string senderEsc = SqlEscape(senderNumber, kPhoneNumberLen);

    char sql[256];
    std::snprintf(sql, sizeof(sql), "SELECT nick FROM phone_numbers WHERE number='%s'", recipEsc.c_str());
    Result* rs = phoneDb_->executeQuery(StringView(sql));
    if (!rs) return;
    char recipNick[25] = {};
    if (rs->getRowCount() > 0) {
        StringView nk = rs->getFieldString(0);
        const size_t len = nk.length() < sizeof(recipNick) - 1 ? nk.length() : sizeof(recipNick) - 1;
        std::memcpy(recipNick, nk.data(), len);
    }
    delete rs;
    if (!recipNick[0]) return;

    SmsReceivedPush push;
    std::memcpy(push.senderNumber, senderNumber, kPhoneNumberLen);
    push.senderNumberLen = kPhoneNumberLen;
    // Display name = the recipient's own contact entry for the sender, else the number.
    std::snprintf(sql, sizeof(sql), "SELECT name FROM phone_contacts WHERE owner='%s' AND number='%s'",
                  recipEsc.c_str(), senderEsc.c_str());
    rs = phoneDb_->executeQuery(StringView(sql));
    if (rs && rs->getRowCount() > 0) {
        StringView nm = rs->getFieldString(0);
        push.senderNameLen = static_cast<uint8_t>(nm.length() <= kMaxPeerName ? nm.length() : kMaxPeerName);
        std::memcpy(push.senderName, nm.data(), push.senderNameLen);
    }
    if (rs) delete rs;
    if (!push.senderNameLen) {
        std::memcpy(push.senderName, senderNumber, kPhoneNumberLen);
        push.senderNameLen = kPhoneNumberLen;
    }
    push.textLen = static_cast<uint8_t>(textLen <= kMaxSmsText ? textLen : kMaxSmsText);
    std::memcpy(push.text, text, push.textLen);
    push.timestamp = ts;
    push.messageId = messageId;
    push.hue = NumberHue(senderNumber, kPhoneNumberLen);

    uint8_t buf[200];
    const size_t n = EncodeSmsReceived(push, buf, sizeof(buf));
    if (n) {
        Response event; event.ownerPush=true; std::memcpy(event.recipient,recipNick,sizeof(event.recipient));
        event.op=Op::SmsReceived; event.size=n; std::memcpy(event.data,buf,n); output.push_back(std::move(event));
    }
}

}
