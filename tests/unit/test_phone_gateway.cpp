#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "phone/phone_net.h"
#include "phone/phone_model.h"
#include "phone_worker.h"
#include "protocol/skrx_phone.h"
#include "protocol/skrx_registry.h"
#include <filesystem>
#include <deque>
#include <set>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>

using namespace skr;
namespace wire=protocol::skrx::phone;
namespace ph=protocol::phone;
namespace api=protocol::api;
namespace {
int checks=0,notifications=0,legacySent=0,gatewaySent=0,submitted=0,bobPushes=0;
phone::Model model; bool viewing=false,failSend=false,bobOnline=true;
std::deque<std::vector<uint8_t>> packets;
std::vector<uint8_t> lastSms,lastPush;
std::vector<phone_backend::Response> completed;
protocol::skrx::Registry registry;
std::unique_ptr<phone_backend::Worker> worker;
phone_backend::Identity alice,bob;
uint32_t caps=api::CapPhone|api::CapGateway|api::CapPhoneGateway|api::CapPhoneSetup;
void Check(bool ok,const char* name) { ++checks; if (!ok) { std::printf("FAIL: %s\n",name); std::exit(1); } }
bool Ingress(const std::vector<uint8_t>& packet,const phone_backend::Identity& identity) {
    api::ClientRequest request;
    if (protocol::skrx::IsGatewayMessage(packet.data(),packet.size())) {
        if (!registry.Find(240,10,protocol::skrx::Direction::Inbound) || !identity.gateway) return false;
        protocol::skrx::GatewayHeader header; const uint8_t* p=nullptr; wire::Frame frame;
        if (!protocol::skrx::DecodeGateway(packet.data(),packet.size(),header,p) ||
            !wire::Decode(header,p,header.payloadLen,true,identity.setup,frame)) return false;
        request.sessionId=frame.sessionId; request.requestId=frame.requestId;
        request.op=frame.op; request.payload=frame.body; request.payloadLen=static_cast<uint8_t>(frame.bodyLen);
    } else {
        if (identity.gateway || !registry.Find(240,9,protocol::skrx::Direction::Inbound) ||
            !api::DecodeClientRequest(packet.data(),packet.size(),request) || request.domain!=api::RequestDomain::Phone) return false;
    }
    if (request.sessionId!=identity.sessionId || !request.requestId ||
        !wire::ValidateRequest(request.op,request.payload,request.payloadLen,identity.setup)) return false;
    phone_backend::Request job; job.identity=identity; job.requestId=request.requestId; job.op=request.op;
    job.size=request.payloadLen; if (job.size) std::memcpy(job.data,request.payload,job.size);
    if (job.op==7 && identity.playerId==1) lastSms=packet;
    if (!worker->Submit(job)) return false;
    ++submitted; return true;
}
void Deliver(const phone_backend::Response& response,const phone_backend::Identity& identity) {
    if (identity.gateway) {
        uint8_t packet[protocol::skrx::kMaxGatewaySize];
        const auto n=wire::Encode(static_cast<uint8_t>(response.op),identity.sessionId,0,response.data,response.size,false,identity.setup,packet,sizeof(packet));
        Check(n!=0,"all production store responses fit the PHONE Gateway");
        if (response.op==api::Op::SmsReceived) lastPush.assign(packet,packet+n);
        protocol::skrx::GatewayHeader header; const uint8_t* p=nullptr;
        Check(protocol::skrx::DecodeGateway(packet,n,header,p),"response envelope decodes");
        phone::net::OnPhoneGatewayMessage(header,p,header.payloadLen);
    } else {
        Check(phone::net::OnPhoneCall(static_cast<uint8_t>(response.op),1,response.data,response.size),"legacy typed response applies");
    }
}
void Pump() {
    phone::net::Tick(GetTickCount64());
    while (!packets.empty()) {
        auto packet=std::move(packets.front()); packets.pop_front();
        Check(Ingress(packet,alice),"selected transport dispatches one request");
    }
    phone_backend::Response response;
    while (worker->Pop(response)) {
        completed.push_back(response);
        if (response.ownerPush) {
            if (!std::strcmp(response.recipient,"Alice")) Deliver(response,alice);
            if (!std::strcmp(response.recipient,"Bob") && bobOnline) ++bobPushes;
        } else if (response.identity.playerId==alice.playerId && response.identity.sessionId==alice.sessionId &&
                   response.identity.generation==alice.generation && response.identity.lease && response.identity.lease->load()) Deliver(response,alice);
    }
}
void Wait(const std::function<bool()>& condition,const char* name) {
    const auto start=GetTickCount64();
    while (!condition() && GetTickCount64()-start<6000) { Pump(); Sleep(2); }
    Check(condition(),name);
}
bool Submit(const phone_backend::Identity& who,uint32_t request,ph::ClientOp op,const uint8_t* data=nullptr,size_t size=0) {
    uint8_t packet[protocol::skrx::kMaxGatewaySize];
    const auto n=wire::Encode(static_cast<uint8_t>(op),who.sessionId,request,data,size,true,who.setup,packet,sizeof(packet));
    return n && Ingress(std::vector<uint8_t>(packet,packet+n),who);
}
bool HasResponse(int id,uint32_t request,api::Op op) {
    for (const auto& r:completed) if (!r.ownerPush && r.identity.playerId==id && r.identity.sessionId==(id==1?alice.sessionId:bob.sessionId) && r.op==op) {
        api::Reader reader(r.data,r.size); if (reader.U32()==request) return true;
    }
    return false;
}
void ProtocolTests() {
    Check(protocol::skrx::RegisterBuiltinEntries(registry),"builtin registry registers");
    const int count=registry.Count();
    Check(!protocol::skrx::RegisterBuiltinEntries(registry) && registry.Count()==count,"no duplicate RPC registration");
    Check(registry.Find(240,10,protocol::skrx::Direction::Inbound) && registry.Find(241,10,protocol::skrx::Direction::Outbound),"exact Gateway registry directions");
    Check(!registry.Find(241,10,protocol::skrx::Direction::Inbound) && !registry.Find(240,10,protocol::skrx::Direction::Outbound),"opposite RPC directions not added");
    Check(registry.Find(243,0,protocol::skrx::Direction::Inbound) && registry.Find(242,0,protocol::skrx::Direction::Outbound),"legacy radar contracts preserved");
    for (uint32_t offered=0;offered<128;++offered) {
        const auto normalized=wire::NormalizeCapabilities(offered);
        const auto selected=wire::SelectTransport(offered);
        Check(selected==wire::Transport::GatewayV1 ? (normalized & (api::CapPhone|api::CapGateway|api::CapPhoneGateway))==(api::CapPhone|api::CapGateway|api::CapPhoneGateway)
             : selected==wire::Transport::Legacy ? (normalized&api::CapPhone)!=0 && !(normalized&api::CapPhoneGateway)
             : !(normalized&api::CapPhone),"explicit PHONE negotiation combinations");
        Check(wire::SelectTransport(wire::NormalizeCapabilities(offered,false))==wire::Transport::Disabled,"disabled service never grants PHONE");
        Check(wire::SelectTransport(wire::NormalizeCapabilities(offered,true,false))!=wire::Transport::GatewayV1,"rollback switch retains only legacy or disabled transport");
    }
    uint8_t packet[500];
    Check(!wire::Encode(7,0,1,nullptr,0,true,false,packet,sizeof(packet)),"zero session rejected");
    Check(!wire::Encode(1,2,0,nullptr,0,true,false,packet,sizeof(packet)),"zero client request rejected");
    Check(!wire::Encode(12,2,1,nullptr,0,true,false,packet,sizeof(packet)),"profile needs setup capability");
    Check(!wire::Encode(8,2,1,nullptr,0,true,true,packet,sizeof(packet)),"unimplemented call opcode not advertised");
    const auto n=wire::Encode(1,2,1,nullptr,0,true,false,packet,sizeof(packet));
    Check(n==15 && packet[0]==0xE0 && packet[1]==10 && packet[2]==5 && packet[3]==1,"PHONE v1 golden envelope");
    protocol::skrx::GatewayHeader header; const uint8_t* p=nullptr; wire::Frame frame;
    Check(protocol::skrx::DecodeGateway(packet,n,header,p) && wire::Decode(header,p,header.payloadLen,true,false,frame),"PHONE request decodes");
    header.version=2; Check(!wire::Decode(header,p,header.payloadLen,true,false,frame),"unknown PHONE version rejected");
    for (size_t len=0;len<n;++len) Check(!protocol::skrx::DecodeGateway(packet,len,header,p),"all truncated envelopes rejected");
    Check(!protocol::skrx::DecodeGateway(packet,n+1,header,p),"trailing byte rejected");
    ph::SmsHistoryResponse history; history.count=2;
    for (auto& item:history.entries) { item.messageId=1; item.textLen=120; std::memset(item.text,'x',120); }
    uint8_t body[400]; auto bodySize=ph::EncodeSmsHistory(history,body,sizeof(body));
    Check(bodySize && wire::Encode(20,2,0,body,bodySize,false,false,packet,sizeof(packet)),"maximum text Gateway history fits");
    history.count=3; bodySize=ph::EncodeSmsHistory(history,body,sizeof(body));
    Check(bodySize && !wire::Encode(20,2,0,body,bodySize,false,false,packet,sizeof(packet)),"legacy history page cannot overflow Gateway");
}
}
namespace skr::phone::input {
Model& GetModel() { return model; }
bool IsVisible() { return false; }
bool IsConversationVisible(const char*) { return viewing; }
void OnSmsNotification(const char*,const char*,const char*,uint8_t,uint64_t) { ++notifications; }
}
namespace skr::game_state {
bool SendEnhancedEnvelope(const uint8_t* p,size_t size) {
    if (protocol::skrx::IsGatewayMessage(p,size)) ++gatewaySent; else ++legacySent;
    if (failSend) return false;
    packets.emplace_back(p,p+size); return true;
}
}
namespace skr::native_api::ui {
void PushNotification(protocol::api::NotifyKind,uint8_t,const char*) { ++notifications; }
}
int main() {
    ProtocolTests();
    const auto directory=std::filesystem::current_path()/"out"/"phone-m58-tests";
    std::filesystem::create_directories(directory);
    const auto path=directory/("phone-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64())+".db");
    worker=std::make_unique<phone_backend::Worker>(path.string());
    Wait([]{ return worker->State()!=0; },"worker initialization completes");
    Check(worker->State()==1,"real SQLite store initialized on worker");
    alice.playerId=1; alice.generation=11; alice.sessionId=100; alice.gateway=true; alice.setup=true; std::strcpy(alice.owner,"Alice");
    bob=alice; bob.playerId=2; bob.generation=12; bob.sessionId=200; std::strcpy(bob.owner,"Bob");
    phone::net::OnPhoneReady(alice.sessionId,true,true);
    Wait([]{ return model.profileLoaded && !model.contactsLoading && !model.threadsLoading; },"Gateway first sync/profile/lists complete");
    Check(!std::strcmp(model.ownNumber,"5550000") && model.profile.stage==0,"existing activation backend reused");
    Check(gatewaySent>0 && legacySent==0,"Gateway session never sends legacy phone requests");
    for (uint8_t stage=1;stage<=4;++stage) {
        Check(phone::net::SaveProfile(stage,1,105,true),"activation step queued");
        Wait([stage]{ return model.profile.stage==stage && !model.profilePending; },"activation acknowledgement persists");
    }
    Check(Submit(bob,1,ph::ClientOp::PhoneSync),"second owner activation queued");
    Wait([]{ return HasResponse(2,1,api::Op::PhoneSync); },"second owner allocation completes");
    ph::PhoneSyncResponse bobNumber;
    for (const auto& response:completed) if (response.identity.playerId==2 && response.op==api::Op::PhoneSync)
        ph::DecodePhoneSync(response.data,response.size,bobNumber);
    Check(!std::strcmp(bobNumber.ownNumber,"5550001"),"unique second number");
    Check(phone::net::AddContact("555 0001","Bob"),"native contact request uses Gateway");
    Wait([]{ return !model.contactOpPending && !model.contactsLoading && model.contactCount==1; },"contact persists and refreshes");
    Check(phone::net::OpenConversation("555 0001","Bob",0),"existing conversation action queued");
    Wait([]{ return !model.convo.loading; },"empty conversation sync completes");
    Check(phone::net::SendSms("555 0001","Native UI SMS"),"native UI send queued without command");
    Wait([]{ return bobPushes==1 && model.convo.count==1 && model.convo.items[0].delivery==phone::Delivery::Delivered; },"SMS store ack and recipient push complete");
    const auto firstId=model.convo.items[0].id;
    Check(firstId!=0 && Ingress(lastSms,alice),"same Gateway SMS request replay accepted for dedup");
    const auto completionCount=completed.size();
    Wait([completionCount]{ return completed.size()>completionCount; },"duplicate request returns original ack");
    Check(bobPushes==1 && model.convo.count==1 && model.convo.items[0].id==firstId,"replay does not insert or push twice");
    ph::SmsSendRequest reply; reply.recipientNumberLen=7; std::strcpy(reply.recipientNumber,"5550000");
    reply.textLen=5; std::strcpy(reply.text,"Reply"); uint8_t body[400];
    auto size=ph::EncodeSmsSend(reply,body,sizeof(body));
    Check(Submit(bob,2,ph::ClientOp::SmsSend,body,size),"second client sends reply");
    Wait([]{ return model.convo.count==2; },"conversation updates without reopening");
    Check(notifications==1,"real incoming notification generated");
    const auto unread=model.threads[0].unread;
    protocol::skrx::GatewayHeader header; const uint8_t* p=nullptr;
    Check(protocol::skrx::DecodeGateway(lastPush.data(),lastPush.size(),header,p),"saved real push frame decodes");
    phone::net::OnPhoneGatewayMessage(header,p,header.payloadLen);
    Check(model.convo.count==2 && notifications==1 && model.threads[0].unread==unread,"duplicate push is idempotent");
    const auto before=submitted;
    auto malformed=lastSms; malformed[3]=99;
    Check(!Ingress(malformed,alice) && submitted==before,"unknown namespace version never enters worker");
    malformed=lastSms; malformed.push_back(0);
    Check(!Ingress(malformed,alice) && submitted==before,"malformed packet never enters worker");
    api::ClientRequest legacy; legacy.sessionId=alice.sessionId; legacy.requestId=900; legacy.domain=api::RequestDomain::Phone; legacy.op=1;
    uint8_t legacyBytes[214]; const auto legacySize=api::EncodeClientRequest(legacy,legacyBytes,sizeof(legacyBytes));
    Check(!Ingress(std::vector<uint8_t>(legacyBytes,legacyBytes+legacySize),alice),"legacy path rejected during Gateway session");
    ph::PhoneSyncResponse fake; fake.ownNumberLen=7; std::strcpy(fake.ownNumber,"5559999");
    size=ph::EncodePhoneSync(fake,body,sizeof(body));
    Check(!phone::net::OnPhoneCall(16,1,body,size) && !std::strcmp(model.ownNumber,"5550000"),"opposite-path server response cannot mutate Gateway model");
    bobOnline=false; worker->Cancel(2);
    Check(phone::net::SendSms("5550001","Offline stored"),"offline recipient send queued");
    Wait([]{ return model.convo.count==3 && model.convo.items[2].delivery==phone::Delivery::Delivered; },"offline message storage acknowledged");
    Check(bobPushes==1,"offline account receives no online push");
    worker->Cancel(1); phone::net::Reset();
    alice.generation=13; alice.sessionId=300; alice.gateway=false;
    phone::net::OnPhoneReady(alice.sessionId,true,false);
    Wait([]{ return model.profileLoaded && !model.contactsLoading && !model.threadsLoading; },"legacy fallback reconnect sync completes");
    Check(!std::strcmp(model.ownNumber,"5550000") && model.profile.stage==4 && model.contactCount==1,"number activation and contacts persist on reconnect");
    Check(legacySent>0 && !Ingress(lastSms,alice),"old Gateway request cannot cross session or transport");
    Check(model.threads[0].unread>0,"unread persisted across reconnect");
    Check(phone::net::OpenConversation("5550001","Bob",0),"legacy history retrieval queued");
    Wait([]{ return !model.convo.loading; },"legacy history retrieved from same database");
    Check(model.convo.count==3,"history persists across transport migration");
    const auto gatewayBefore=gatewaySent;
    Check(phone::net::SendSms("5550001","Legacy fallback"),"native UI works on legacy fallback");
    Wait([]{ return model.convo.count==4 && model.convo.items[3].delivery==phone::Delivery::Delivered; },"legacy send ack completes");
    Check(gatewaySent==gatewayBefore,"legacy session does not dual-send Gateway SMS");
    worker->Cancel(1); phone::net::Reset(); alice.generation=14; alice.sessionId=400; alice.gateway=true;
    phone::net::OnPhoneReady(alice.sessionId,true,true);
    Wait([]{ return model.profileLoaded; },"Gateway can reconnect after legacy session");
    const auto legacyBefore=legacySent;
    Check(phone::net::OpenConversation("5550001","Bob",0),"Gateway history queued");
    Wait([]{ return !model.convo.loading; },"two-message Gateway pages merge full history");
    Check(model.convo.count==4,"Gateway history pagination does not lose messages");
    bobOnline=true; bob.generation=99; bob.sessionId=201;
    Check(Submit(bob,1,ph::ClientOp::PhoneSync),"offline recipient reconnect sync queued");
    Wait([]{ return HasResponse(2,1,api::Op::PhoneSync); },"recipient reconnect recognized");
    ph::SmsHistoryRequest bobHistory; bobHistory.peerNumberLen=7; std::strcpy(bobHistory.peerNumber,"5550000");
    size=ph::EncodeSmsHistoryReq(bobHistory,body,sizeof(body));
    Check(Submit(bob,3,ph::ClientOp::SmsHistory,body,size),"offline recipient history queued");
    Wait([]{ return HasResponse(2,3,api::Op::SmsHistory); },"offline recipient history retrieved");
    bool offlineFound=false;
    for (const auto& response:completed) if (response.identity.playerId==2 && response.op==api::Op::SmsHistory) {
        ph::SmsHistoryResponse page;
        if (ph::DecodeSmsHistory(response.data,response.size,page))
            for (unsigned i=0;i<page.count;++i) offlineFound=offlineFound || !std::strcmp(page.entries[i].text,"Offline stored");
    }
    Check(offlineFound,"offline SMS survives until recipient reconnect");
    failSend=true;
    Check(phone::net::SendSms("5550001","Must not dual-send"),"test failed send queued");
    phone::net::Tick(GetTickCount64()+500);
    phone::net::Tick(GetTickCount64()+10000);
    Check(legacySent==legacyBefore,"failed Gateway send never falls back mid-session");
    Check(model.convo.items[model.convo.count-1].delivery==phone::Delivery::Failed,"failed send reports failure");
    failSend=false; phone::net::Reset(); worker->Stop(); worker.reset();
    phone_backend::Database db; Check(db.Open(path.string()),"reopen persistent database");
    std::unique_ptr<phone_backend::Result> result(db.executeQuery("SELECT COUNT(*),COUNT(DISTINCT number) FROM phone_numbers"));
    Check(result && result->getFieldInt(0)==2 && result->getFieldInt(1)==2,"reconnect has exactly one allocation per owner");
    result.reset(db.executeQuery("SELECT COUNT(*) FROM phone_messages"));
    Check(result && result->getFieldInt(0)==4,"replay and failed transport produced no extra stored messages");
    result.reset(); db.Close(); std::filesystem::remove(path);
    std::printf("phone_gateway: %d checks passed\n",checks); return 0;
}
