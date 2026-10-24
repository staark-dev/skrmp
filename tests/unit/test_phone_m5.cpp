#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "phone/phone_input.h"
#include "phone/phone_net.h"
#include "protocol/phone_protocol.h"
#include "protocol/phone_profile.h"
#include <vector>
#include <cstdio>
#include <cstring>
#include <cstdlib>

using namespace skr;
namespace { phone::Model model; bool visible=false, viewing=false; int notices=0, checks=0;
std::vector<std::vector<uint8_t>> sent;
void Check(bool ok,const char* name) { ++checks; if (!ok) { std::printf("FAIL: %s\n",name); std::exit(1); } }
uint64_t now=0;
void Drain() { for (int i=0;i<6;++i) phone::net::Tick(now+=120); }
protocol::api::ClientRequest Last(protocol::phone::ClientOp op) {
    protocol::api::ClientRequest req;
    for (auto it=sent.rbegin();it!=sent.rend();++it)
        if (protocol::api::DecodeClientRequest(it->data(),it->size(),req) && req.op==static_cast<uint8_t>(op)) return req;
    Check(false,"expected outgoing request"); return req;
}
void Sync() {
    Drain(); protocol::phone::PhoneSyncResponse response;
    response.requestId=Last(protocol::phone::ClientOp::PhoneSync).requestId;
    std::memcpy(response.ownNumber,"5550000",8); response.ownNumberLen=7;
    uint8_t buf[64]; const auto n=protocol::phone::EncodePhoneSync(response,buf,sizeof(buf));
    Check(phone::net::OnPhoneCall(16,1,buf,n),"sync dispatch"); Drain();
}
void ApplyProfile(protocol::phone::Profile p,uint32_t req) {
    p.requestId=req; uint8_t buf[16]; const auto n=protocol::phone::EncodeProfile(p,buf,sizeof(buf));
    Check(n==9,"profile exact size"); phone::net::OnPhoneCall(25,2,buf,n);
}
}
namespace skr::phone::input {
Model& GetModel() { return model; }
bool IsVisible() { return visible; }
bool IsConversationVisible(const char*) { return viewing; }
void OnSmsNotification(const char*,const char*,const char*,uint8_t,uint64_t) { ++notices; }
}
namespace skr::game_state {
bool SendEnhancedEnvelope(const uint8_t* p,size_t n) { sent.emplace_back(p,p+n); return true; }
}
namespace skr::native_api::ui {
void PushNotification(protocol::api::NotifyKind,uint8_t,const char*) { ++notices; }
}
int main() {
    using namespace protocol::phone;
    now=GetTickCount64();
    char number[8], display[16];
    Check(IsM5PhoneNumber("5550000",7) && IsM5PhoneNumber("5559999",7),"pool boundaries");
    Check(!IsM5PhoneNumber("5549999",7) && !IsM5PhoneNumber("5560000",7),"outside pool");
    Check(IsValidPhoneNumber("1000000",7),"legacy numbers retained");
    Check(!IsValidPhoneNumber(nullptr,7),"null rejected");
    Check(NormalizePhoneNumber(" 555 0000 ",number) && !std::strcmp(number,"5550000"),"spaces normalized");
    Check(!NormalizePhoneNumber("555-0000",number) && !NormalizePhoneNumber("55500000",number),"invalid recipient rejected");
    FormatPhoneNumber("5559999",display); Check(!std::strcmp(display,"555 9999"),"display format");
    uint8_t buf[64]; Profile p; // profile codec rejects all truncations and trailing bytes
    const auto n=EncodeProfile(p,buf,sizeof(buf));
    for (size_t len=0;len<n;++len) { protocol::phone::Profile out; Check(!DecodeProfile(buf,len,out),"truncated profile rejected"); }
    protocol::phone::Profile out; Check(!DecodeProfile(buf,n+1,out),"profile trailing byte rejected");
    p.stage=5; Check(!EncodeProfile(p,buf,sizeof(buf)),"unknown setup stage rejected"); p.stage=0;
    p.language=1; Check(!EncodeProfile(p,buf,sizeof(buf)),"unsupported language rejected"); p.language=0;
    p.scale=79; Check(!EncodeProfile(p,buf,sizeof(buf)),"invalid scale rejected"); p.scale=100;
    phone::net::OnPhoneReady(42,true); Sync();
    Check(model.setupSupported && !model.profileLoaded && model.profilePending,"M5 waits for authoritative profile");
    const auto request=Last(ClientOp::ProfileGet).requestId;
    ApplyProfile(p,request+100); Check(!model.profileLoaded,"stale profile ignored");
    ApplyProfile(p,request); Check(model.profileLoaded && model.profile.stage==0,"first activation welcome");
    Check(phone::net::SaveProfile(1,1,110,false),"save queued");
    Check(model.profile.stage==0 && model.profilePending,"no optimistic activation advance");
    Check(!phone::net::SaveProfile(1,1,110,false),"double click bounded"); Drain();
    p.stage=1; p.wallpaper=1; p.scale=110; p.banners=0;
    ApplyProfile(p,Last(ClientOp::ProfileSave).requestId);
    Check(model.profile.stage==1 && model.profile.wallpaper==1 && !model.settings.banners,"acknowledged profile applied");
    Check(model.settings.uiScale>1.09f && model.settings.uiScale<1.11f,"saved size applied");
    SmsReceivedPush sms; sms.messageId=81; sms.senderNumberLen=7; std::memcpy(sms.senderNumber,"5559999",8);
    sms.senderNameLen=4; std::memcpy(sms.senderName,"Test",5); sms.textLen=2; std::memcpy(sms.text,"Hi",3);
    auto smsLen=EncodeSmsReceived(sms,buf,sizeof(buf)); Check(smsLen>0,"real push encodes");
    phone::net::OnPhoneCall(22,3,buf,smsLen); phone::net::OnPhoneCall(22,4,buf,smsLen);
    Check(model.threadCount==1 && model.threads[0].unread==1,"duplicate push does not duplicate unread");
    Check(notices==0,"disabled banners respected for closed phone");
    Check(phone::net::OpenConversation("555 9999","Test",0),"formatted recipient opens real conversation");
    Drain();
    const auto historyRequest=Last(ClientOp::SmsHistory).requestId;
    sms.messageId=82;
    smsLen=EncodeSmsReceived(sms,buf,sizeof(buf));
    phone::net::OnPhoneCall(22,5,buf,smsLen);
    SmsHistoryResponse history; history.requestId=historyRequest; history.count=1;
    history.entries[0].messageId=81; history.entries[0].direction=SmsDirection::Incoming;
    history.entries[0].textLen=2; std::memcpy(history.entries[0].text,"Hi",3);
    const auto historyLen=EncodeSmsHistory(history,buf,sizeof(buf));
    Check(historyLen>0,"history page encodes");
    phone::net::OnPhoneCall(20,6,buf,historyLen);
    Check(model.convo.count==2 && model.convo.items[0].id==81 && model.convo.items[1].id==82,"history merge preserves racing live push in order");
    viewing=true; phone::net::MarkConversationRead(); Drain();
    const auto readRequest=Last(ClientOp::MarkRead).requestId;
    protocol::api::Writer readAck(buf,sizeof(buf)); readAck.U32(readRequest);
    phone::net::OnPhoneCall(26,7,buf,readAck.size());
    Check(model.threads[0].unread==0,"visible conversation queues persistent read watermark");
    phone::net::MarkConversationRead();
    viewing=false;
    phone::net::Reset(); Check(!model.profileLoaded && !model.ownNumber[0] && model.settings.uiScale==1.0f,"identity reset clears profile");
    sent.clear(); now=GetTickCount64(); phone::net::OnPhoneReady(43,false); Sync();
    Check(!model.setupSupported && !model.profilePending,"M4 fallback has no onboarding");
    Check(!phone::net::RequestProfile(),"M5 request blocked without capability");
    for (const auto& packet:sent) { protocol::api::ClientRequest req; Check(protocol::api::DecodeClientRequest(packet.data(),packet.size(),req) && req.op<12,"legacy server receives M4 ops only"); }
    phone::net::Reset(); sent.clear(); now=GetTickCount64(); phone::net::OnPhoneReady(44,true); Sync();
    Drain(); phone::net::Tick(now+10000);
    Check(!model.profilePending && !model.profileLoaded,"profile timeout releases pending state");
    Check(phone::net::RequestProfile(),"profile can retry after unavailable service");
    std::printf("phone_m5: %d checks passed\n",checks); return 0;
}
