// PHONE M6 — offscreen native preview of the SKR Enhanced phone
// on a real Win32 Direct3D9 HAL device through the same ImGui DX9 backend and
// font faces the native pause renderer loads in game.
//
// PRODUCTION code drawn here (linked from client/phone/):
//   phone_layout.h       ComputeLayout          (measured device geometry)
//   phone_controller.cpp Controller             (state machine + animation timing)
//   phone_renderer.cpp   Draw                   (shell, pages, transitions, overlays)
// PREVIEW-ONLY: the backdrop is an operator-supplied development screenshot
// (stretched/cropped to each resolution), and every phone value is
// sample data (Model::previewData = true, disclosed in the external caption).
// Nothing here talks to GTA, SA-MP or the gamemode.
//
// Usage: SKRPhonePreview <outDir> <backdrop.png>
// Writes PNGs + report.txt (layout rects, per-screen draw cost). Exit 0 = all written.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <wincodec.h>

#include <imgui.h>
#include <backends/imgui_impl_dx9.h>

#include "phone/phone_controller.h"
#include "phone/phone_layout.h"
#include "phone/phone_model.h"
#include "phone/phone_renderer.h"
#include "phone/phone_text.h"
#include "phone/phone_wallpaper.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace skr::phone;

namespace {

IWICImagingFactory* g_wic = nullptr;
FILE* g_report = nullptr;
std::string g_out;

void Report(const char* fmt, ...) {
    va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a);
    if (g_report) { va_start(a, fmt); vfprintf(g_report, fmt, a); va_end(a); }
}

std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

bool LoadBgra(const std::string& path, std::vector<uint32_t>& px, UINT& w, UINT& h) {
    IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* fr = nullptr; IWICFormatConverter* cv = nullptr;
    bool ok = SUCCEEDED(g_wic->CreateDecoderFromFilename(Wide(path).c_str(), nullptr, GENERIC_READ,
                                                         WICDecodeMetadataCacheOnDemand, &dec)) &&
              SUCCEEDED(dec->GetFrame(0, &fr)) && SUCCEEDED(g_wic->CreateFormatConverter(&cv)) &&
              SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                       WICBitmapPaletteTypeCustom)) &&
              SUCCEEDED(cv->GetSize(&w, &h));
    if (ok) {
        px.resize(static_cast<size_t>(w) * h);
        ok = SUCCEEDED(cv->CopyPixels(nullptr, w * 4, w * h * 4, reinterpret_cast<BYTE*>(px.data())));
    }
    if (cv) cv->Release();
    if (fr) fr->Release();
    if (dec) dec->Release();
    return ok;
}

bool SavePng(const std::string& path, const uint32_t* px, UINT w, UINT h) {
    IWICStream* st = nullptr; IWICBitmapEncoder* enc = nullptr; IWICBitmapFrameEncode* fr = nullptr;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    bool ok = SUCCEEDED(g_wic->CreateStream(&st)) &&
              SUCCEEDED(st->InitializeFromFilename(Wide(path).c_str(), GENERIC_WRITE)) &&
              SUCCEEDED(g_wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
              SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) &&
              SUCCEEDED(enc->CreateNewFrame(&fr, nullptr)) && SUCCEEDED(fr->Initialize(nullptr)) &&
              SUCCEEDED(fr->SetSize(w, h)) && SUCCEEDED(fr->SetPixelFormat(&fmt)) &&
              SUCCEEDED(fr->WritePixels(h, w * 4, w * h * 4, reinterpret_cast<BYTE*>(const_cast<uint32_t*>(px)))) &&
              SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit());
    if (fr) fr->Release();
    if (enc) enc->Release();
    if (st) st->Release();
    return ok;
}

bool Capture(IDirect3DDevice9* dev, UINT w, UINT h, std::vector<uint32_t>& px) {
    IDirect3DSurface9* bb = nullptr; IDirect3DSurface9* sys = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return false;
    D3DSURFACE_DESC d; bb->GetDesc(&d);
    bool ok = SUCCEEDED(dev->CreateOffscreenPlainSurface(w, h, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) &&
              SUCCEEDED(dev->GetRenderTargetData(bb, sys));
    D3DLOCKED_RECT lr{};
    if (ok && SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
        px.resize(static_cast<size_t>(w) * h);
        for (UINT y = 0; y < h; ++y) {
            const uint32_t* row = reinterpret_cast<const uint32_t*>(static_cast<const BYTE*>(lr.pBits) + y * lr.Pitch);
            for (UINT x = 0; x < w; ++x) px[y * w + x] = row[x] | 0xFF000000u;
        }
        sys->UnlockRect();
    } else ok = false;
    if (sys) sys->Release();
    bb->Release();
    return ok;
}

void SaveCrop(const std::string& path, const std::vector<uint32_t>& px, UINT w, UINT h, Rect r, int k) {
    const int x0 = std::max(0, static_cast<int>(r.x0)), y0 = std::max(0, static_cast<int>(r.y0));
    const int cw = std::min(static_cast<int>(r.x1), static_cast<int>(w)) - x0;
    const int ch = std::min(static_cast<int>(r.y1), static_cast<int>(h)) - y0;
    if (cw <= 0 || ch <= 0) return;
    std::vector<uint32_t> out(static_cast<size_t>(cw * k) * ch * k);
    for (int y = 0; y < ch * k; ++y)
        for (int x = 0; x < cw * k; ++x)
            out[static_cast<size_t>(y) * cw * k + x] = px[static_cast<size_t>(y0 + y / k) * w + (x0 + x / k)];
    SavePng(path, out.data(), cw * k, ch * k);
}

// ── Sample data (PREVIEW) ──────────────────────────────────────────────
void FillPreviewModel(Model& m) {
    m = Model{};
    m.previewData = true;
    m.sync = SyncState::Ready;
    CopyText(m.ownNumber, "1000000");
    CopyText(m.status.time, "21:47");
    CopyText(m.status.date, "Saturday, 24 October");
    m.status.signal = 4;
    m.status.battery = 100;

    static const struct { const char* n; const char* num; uint8_t hue; } kContacts[] = {
        {"Big Smoke", "1000017", 2}, {"Catalina", "1000063", 3}, {"Cesar Vialpando", "1000021", 5},
        {"Kendl Johnson", "1000009", 4}, {"Madd Dogg", "1000088", 6}, {"Mike Toreno", "1000040", 7},
        {"OG Loc", "1000033", 0}, {"Ryder", "1000004", 1}, {"SKR_Player", "1000001", 1},
        {"Sweet", "1000008", 0}, {"The Truth", "1000069", 5}, {"Woozie", "1000077", 4},
    };
    for (const auto& k : kContacts) {
        Contact& c = m.contacts[m.contactCount++];
        CopyText(c.name, k.n); CopyText(c.number, k.num); c.hue = k.hue;
    }
    static const struct { const char* p; const char* num; const char* t; const char* time; uint8_t unread; uint8_t hue; } kThreads[] = {
        {"SKR_Player", "1000001", "Meet me at Grove Street — bring the Sultan.", "21:44", 2, 1},
        {"Cesar Vialpando", "1000021", "Race tonight at the docks?", "20:12", 0, 5},
        {"Kendl Johnson", "1000009", "Call me when you get this", "23.10", 0, 4},
        {"1000052", "1000052", "Who is this?", "22.10", 0, 6},
    };
    for (const auto& k : kThreads) {
        ThreadSummary& t = m.threads[m.threadCount++];
        CopyText(t.peer, k.p); CopyText(t.number, k.num); CopyText(t.preview, k.t); CopyText(t.time, k.time);
        t.unread = k.unread; t.hue = k.hue;
    }
    Conversation& cv = m.convo;
    CopyText(cv.peer, "SKR_Player"); CopyText(cv.number, "1000001"); cv.hue = 1;
    cv.hasMore = true; cv.pages = 5;
    static const struct { const char* t; const char* time; Delivery d; } kMsgs[] = {
        {"Yo, where you at?", "21:38", Delivery::Received},
        {"Just left Idlewood. What's up?", "21:39", Delivery::Delivered},
        {"Ballas rolled past the house again. Need you here.", "21:43", Delivery::Received},
        {"Meet me at Grove Street.", "21:44", Delivery::Received},
        {"On my way. Setting the GPS now.", "21:46", Delivery::Delivered},
        {"Be there in 2 minutes", "21:47", Delivery::Sending},
    };
    for (const auto& k : kMsgs) {
        Message& msg = cv.items[cv.count++];
        CopyText(msg.text, k.t); CopyText(msg.time, k.time); msg.delivery = k.d;
    }
    GpsInfo& g = m.gps;
    g.status = NavStatus::Navigating;
    CopyText(g.destName, "Grove Street");
    CopyText(g.destArea, "Ganton, Los Santos");
    g.remainingM = 1800;
    static const struct { const char* n; const char* a; float d; } kDest[] = {
        {"Grove Street", "Ganton, Los Santos", 1800}, {"Ten Green Bottles", "Ganton, Los Santos", 2050},
        {"Pay 'n' Spray", "Idlewood, Los Santos", 1150}, {"All Saints General", "Market, Los Santos", 3400},
        {"LS International", "Los Santos Airport", 4600}, {"Four Dragons Casino", "The Strip, Las Venturas", 21300},
    };
    for (const auto& k : kDest) {
        Destination& d = g.list[g.count++];
        CopyText(d.name, k.n); CopyText(d.area, k.a); d.distanceM = k.d;
    }
    CopyText(m.caller.name, "Big Smoke"); CopyText(m.caller.number, "1000017"); m.caller.hue = 2;
    m.banner.app = AppId::Messages;
    CopyText(m.banner.title, "SKR_Player");
    CopyText(m.banner.text, "Meet me at Grove Street.");
    CopyText(m.banner.number, "1000001");
    m.settings.uiScale = 1.0f;
    CopyText(m.settings.openKey, "L");
}

// ── Scenes ─────────────────────────────────────────────────────────────
enum class Hover : uint8_t { None, App, Contact, Thread, Destination, Accept, Back, Button };

struct Scene {
    const char* name;
    void (*setup)(Controller&, uint64_t& now);
    Hover hover = Hover::None;
    uint8_t hoverIndex = 0;
    bool press = false;
    void (*prep)(Model&, Views&) = nullptr;     // M4F: page sub-state, typed text, focus
};

void DetailSweet(Model&, Views& v) {
    v.contacts.sub = ContactsSub::Details;
    CopyText(v.contacts.detailNumber, "1000008");
}

constexpr uint64_t kSettle = 5000;
void Settle(Controller& c, uint64_t& now) { now += kSettle; c.Update(now); }
void OpenHome(Controller& c, uint64_t& now) { now = 1000; c.Open(now); Settle(c, now); }
void OpenApp(Controller& c, uint64_t& now, AppId a) { OpenHome(c, now); c.OpenApp(a, now); Settle(c, now); }

const Scene kScenes[] = {
    {"01_closed", [](Controller& c, uint64_t& now) { now = 1000; c.Update(now); }},
    {"02a_opening_90ms", [](Controller& c, uint64_t& now) { now = 1000; c.Open(now); now += 90; c.Update(now); }},
    {"02b_opening_170ms", [](Controller& c, uint64_t& now) { now = 1000; c.Open(now); now += 170; c.Update(now); }},
    {"03_home", OpenHome},
    {"03b_home_hover_messages", OpenHome, Hover::App, static_cast<uint8_t>(AppId::Messages)},
    {"04_contacts", [](Controller& c, uint64_t& now) { OpenApp(c, now, AppId::Contacts); }, Hover::Contact, 2},
    {"05a_messages_threads", [](Controller& c, uint64_t& now) { OpenApp(c, now, AppId::Messages); }, Hover::Thread, 0},
    {"05b_messages_conversation", [](Controller& c, uint64_t& now) {
        OpenApp(c, now, AppId::Messages); c.OpenPage(1, now); Settle(c, now); }},
    {"06_incoming_call", [](Controller& c, uint64_t& now) { OpenHome(c, now); c.BeginIncomingCall(now); now += 420; c.Update(now); },
     Hover::Accept, 0},
    {"07_gps", [](Controller& c, uint64_t& now) { OpenApp(c, now, AppId::Gps); }, Hover::Destination, 2},
    {"08_settings", [](Controller& c, uint64_t& now) { OpenApp(c, now, AppId::Settings); }},
    {"09_my_number", [](Controller& c, uint64_t& now) { OpenApp(c, now, AppId::Phone); }},
    {"10a_transition_home_to_gps_45pct", [](Controller& c, uint64_t& now) {
        OpenHome(c, now); c.OpenApp(AppId::Gps, now); now += 70; c.Update(now); }},
    {"10b_transition_threads_to_chat_50pct", [](Controller& c, uint64_t& now) {
        OpenApp(c, now, AppId::Messages); c.OpenPage(1, now); now += 45; c.Update(now); }},
    {"11_notification_banner", [](Controller& c, uint64_t& now) { OpenHome(c, now); c.PushNotification(now); now += 600; c.Update(now); }},
    {"12_closing_120ms", [](Controller& c, uint64_t& now) { OpenHome(c, now); c.Close(now); now += 120; c.Update(now); }},
    // PHONE-M4F screens.
    {"14_contact_detail", [](Controller& c, uint64_t& now) { OpenApp(c, now, AppId::Contacts); c.OpenPage(1, now); Settle(c, now); },
     Hover::None, 0, false, DetailSweet},
    {"15_contact_add_typing", [](Controller& c, uint64_t& now) { OpenApp(c, now, AppId::Contacts); c.OpenPage(1, now); Settle(c, now); },
     Hover::None, 0, false, [](Model&, Views& v) {
        v.contacts.sub = ContactsSub::Add;
        TextSet(v.contacts.name, "Ryder", kFieldNameMax);
        TextSet(v.contacts.number, "10000", kFieldNumberMax);
        v.focus = Field::ContactNumber; }},
    {"16_contact_remove_confirm", [](Controller& c, uint64_t& now) { OpenApp(c, now, AppId::Contacts); c.OpenPage(1, now); Settle(c, now); },
     Hover::None, 0, false, [](Model& m, Views& v) { DetailSweet(m, v); v.contacts.confirmRemove = true; }},
    {"17_contacts_search", [](Controller& c, uint64_t& now) { OpenApp(c, now, AppId::Contacts); },
     Hover::None, 0, false, [](Model&, Views& v) { TextSet(v.contacts.search, "sk", kFieldSearchMax); v.focus = Field::ContactSearch; }},
    {"18_new_message", [](Controller& c, uint64_t& now) { OpenApp(c, now, AppId::Messages); c.OpenPage(1, now); Settle(c, now); },
     Hover::None, 0, false, [](Model&, Views& v) {
        v.messages.sub = MessagesSub::Compose; TextSet(v.messages.to, "1000", kFieldNumberMax); v.focus = Field::NewMessageTo; }},
    {"19_conversation_typing", [](Controller& c, uint64_t& now) { OpenApp(c, now, AppId::Messages); c.OpenPage(1, now); Settle(c, now); },
     Hover::None, 0, false, [](Model&, Views& v) {
        TextSet(v.messages.draft, "Bring the car around, I'm outside", kFieldSmsMax); v.focus = Field::SmsDraft; }},
    {"20_contacts_empty", [](Controller& c, uint64_t& now) { OpenApp(c, now, AppId::Contacts); },
     Hover::None, 0, false, [](Model& m, Views&) { m.contactCount = 0; }},
    {"21_messages_loading", [](Controller& c, uint64_t& now) { OpenApp(c, now, AppId::Messages); },
     Hover::None, 0, false, [](Model& m, Views&) { m.threadCount = 0; m.threadsLoading = true; }},
    {"22_phone_not_logged_in", [](Controller& c, uint64_t& now) { OpenApp(c, now, AppId::Phone); },
     Hover::None, 0, false, [](Model& m, Views&) {
        m.sync = SyncState::Error; m.ownNumber[0] = 0; CopyText(m.syncError, "Log in to use your phone"); }},
    {"23_messages_no_service", [](Controller& c, uint64_t& now) { OpenApp(c, now, AppId::Messages); },
     Hover::None, 0, false, [](Model& m, Views&) { m.sync = SyncState::Offline; m.threadCount = 0; }},
    {"24_sms_banner_over_contacts", [](Controller& c, uint64_t& now) {
        OpenApp(c, now, AppId::Contacts); c.PushNotification(now); now += 600; c.Update(now); }},
    {"25_send_failed_notice", [](Controller& c, uint64_t& now) { OpenApp(c, now, AppId::Messages); c.OpenPage(1, now); Settle(c, now); },
     Hover::None, 0, false, [](Model& m, Views&) {
        m.convo.items[m.convo.count - 1].delivery = Delivery::Failed;
        CopyText(m.notice, "Recipient not found"); m.noticeError = true; m.noticeAt = 1; }},
    {"m5_0_welcome", OpenHome, Hover::None,0,false,[](Model& m,Views&) {
        m.setupSupported=true; m.profileLoaded=true; m.profile.stage=0;
        CopyText(m.ownNumber,"5550000"); } },
    {"m5_1_language", OpenHome, Hover::None,0,false,[](Model& m,Views&) {
        m.setupSupported=true; m.profileLoaded=true; m.profile.stage=1;
        CopyText(m.ownNumber,"5550000"); } },
    {"m5_2_number", OpenHome, Hover::None,0,false,[](Model& m,Views&) {
        m.setupSupported=true; m.profileLoaded=true; m.profile.stage=2;
        CopyText(m.ownNumber,"5550000"); } },
    {"m5_3_personalization", OpenHome, Hover::None,0,false,[](Model& m,Views&) {
        m.setupSupported=true; m.profileLoaded=true; m.profile.stage=3;
        CopyText(m.ownNumber,"5550000"); } },
    {"m5_service_unavailable",OpenHome,Hover::None,0,false,[](Model& m,Views&) {
        m.setupSupported=true; m.profileLoaded=false; m.sync=SyncState::Error; }},
    {"m5_settings",[](Controller& c,uint64_t& now) { OpenApp(c,now,AppId::Settings); },Hover::None,0,false,[](Model& m,Views&) {
        m.setupSupported=true; m.profileLoaded=true; m.profile.stage=4; CopyText(m.ownNumber,"5559999"); }},
    {"m5_gps_idle",[](Controller& c,uint64_t& now) { OpenApp(c,now,AppId::Gps); },Hover::None,0,false,[](Model& m,Views&) {
        m.previewData=false; m.gps={}; }},
};

struct Frame {
    UINT W, H;
    IDirect3DDevice9* dev;
    IDirect3DTexture9* backdrop;
    UINT bw, bh;
    Fonts fonts;
};

void DrawBackdrop(const Frame& f) {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    // Cover-fit the screenshot (crop the overflowing axis, never letterbox).
    const float sa = static_cast<float>(f.W) / f.H, ia = static_cast<float>(f.bw) / f.bh;
    ImVec2 uv0(0, 0), uv1(1, 1);
    if (sa > ia) { const float k = ia / sa; uv0.y = (1 - k) * 0.5f; uv1.y = 1 - uv0.y; }
    else { const float k = sa / ia; uv0.x = (1 - k) * 0.5f; uv1.x = 1 - uv0.x; }
    dl->AddImage(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(f.backdrop)), ImVec2(0, 0),
                 ImVec2(static_cast<float>(f.W), static_cast<float>(f.H)), uv0, uv1);
}

void Caption(const Frame& f, const char* text) {
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float s = 13.0f * f.H / 768.0f;
    ImFont* font = f.fonts.body[1] ? f.fonts.body[1] : ImGui::GetFont();
    const ImVec2 sz = font->CalcTextSizeA(s, FLT_MAX, 0, text);
    const float x = (f.W - sz.x) * 0.5f, y = f.H - sz.y - 8.0f * f.H / 768.0f;
    dl->AddRectFilled(ImVec2(x - 8, y - 3), ImVec2(x + sz.x + 8, y + sz.y + 3), IM_COL32(0, 0, 0, 150), 4);
    dl->AddText(font, s, ImVec2(x, y), IM_COL32(242, 178, 64, 255), text);
}

Pointer PointerFor(const FrameHits& hits, Hover h, uint8_t idx) {
    Action want = Action::None;
    switch (h) {
        case Hover::App: want = Action::OpenApp; break;
        case Hover::Contact: want = Action::OpenContact; break;
        case Hover::Thread: want = Action::OpenThread; break;
        case Hover::Destination: want = Action::SelectDestination; break;
        case Hover::Accept: want = Action::AcceptCall; break;
        case Hover::Back: want = Action::Back; break;
        default: return {};
    }
    for (int i = hits.count - 1; i >= 0; --i)
        if (hits.items[i].action == want && hits.items[i].index == idx)
            return {hits.items[i].rect.Cx(), hits.items[i].rect.Cy(), false};
    return {};
}

bool RenderScene(const Frame& f, const Scene& sc, const char* tag, float uiScale, bool crop) {
    Model m;
    FillPreviewModel(m);
    m.settings.uiScale = uiScale;
    Views views;
    Controller c;
    uint64_t now = 0;
    sc.setup(c, now);
    if (sc.prep) sc.prep(m, views);
    if (m.noticeAt) m.noticeAt = now - 600;     // notice mid-display

    LayoutInput in;
    in.screenW = static_cast<float>(f.W); in.screenH = static_cast<float>(f.H);
    in.uiScale = uiScale; in.slide = c.Slide();
    const Layout L = ComputeLayout(in);

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(f.W), static_cast<float>(f.H));
    io.DeltaTime = 1.0f / 60.0f;

    // Pass 1 (discarded): collect hit regions to place the preview pointer.
    FrameHits hits;
    ImGui_ImplDX9_NewFrame();
    ImGui::NewFrame();
    Draw(ImGui::GetForegroundDrawList(), L, c, m, views, f.fonts, Pointer{}, now, &hits);
    ImGui::EndFrame();
    const Pointer ptr = PointerFor(hits, sc.hover, sc.hoverIndex);

    f.dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
    f.dev->BeginScene();
    ImGui_ImplDX9_NewFrame();
    ImGui::NewFrame();
    DrawBackdrop(f);
    Draw(ImGui::GetForegroundDrawList(), L, c, m, views, f.fonts, ptr, now, &hits);
    char cap[160];
    std::snprintf(cap, sizeof(cap), "PHONE-M6 PREVIEW  \xC2\xB7  representative offline data  \xC2\xB7  native ImGui/D3D9  \xC2\xB7  %s  \xC2\xB7  %ux%u  scale %.2f",
                  sc.name, f.W, f.H, uiScale);
    Caption(f, cap);
    ImGui::EndFrame();
    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
    f.dev->EndScene();

    std::vector<uint32_t> px;
    if (!Capture(f.dev, f.W, f.H, px)) return false;
    char path[MAX_PATH];
    std::snprintf(path, sizeof(path), "%s\\%s_%ux%u.png", g_out.c_str(), tag, f.W, f.H);
    const bool ok = SavePng(path, px.data(), f.W, f.H);
    if (crop && c.Visible()) {
        const float pad = 26.0f * L.u;
        std::snprintf(path, sizeof(path), "%s\\%s_%ux%u_zoom2x.png", g_out.c_str(), tag, f.W, f.H);
        SaveCrop(path, px, f.W, f.H, {L.body.x0 - pad, L.body.y0 - pad, L.body.x1 + pad, L.body.y1 + pad}, 2);
    }
    Report("  %-40s %ux%u scale %.2f state %d slide %.3f alpha %.2f body (%.0f,%.0f)-(%.0f,%.0f) %s\n", tag, f.W, f.H,
           uiScale, static_cast<int>(c.GetState()), c.Slide(), c.Alpha(), L.body.x0, L.body.y0, L.body.x1, L.body.y1,
           ok ? "ok" : "FAILED");
    return ok;
}

// CPU cost of building the phone's draw list (no GPU), per screen.
void MeasureCost(const Frame& f) {
    Model m;
    FillPreviewModel(m);
    LayoutInput in;
    in.screenW = static_cast<float>(f.W); in.screenH = static_cast<float>(f.H);
    const Layout L = ComputeLayout(in);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(f.W), static_cast<float>(f.H));
    LARGE_INTEGER freq; QueryPerformanceFrequency(&freq);
    struct { const char* name; AppId app; uint8_t page; bool call; } kCases[] = {
        {"home", AppId::Home, 0, false}, {"contacts", AppId::Contacts, 0, false},
        {"conversation", AppId::Messages, 1, false}, {"gps", AppId::Gps, 0, false},
        {"incoming call", AppId::Home, 0, true},
    };
    Report("\n  draw-list build cost (%ux%u, CPU only, 2000 frames each):\n", f.W, f.H);
    for (const auto& k : kCases) {
        Controller c;
        uint64_t now = 1000;
        OpenHome(c, now);
        if (k.app != AppId::Home) { c.OpenApp(k.app, now); Settle(c, now); }
        if (k.page) { c.OpenPage(k.page, now); Settle(c, now); }
        if (k.call) c.BeginIncomingCall(now);
        Views views;
        FrameHits hits;
        int vtx = 0, idx = 0;
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        for (int i = 0; i < 2000; ++i) {
            ImGui_ImplDX9_NewFrame();
            ImGui::NewFrame();
            ImDrawList* dl = ImGui::GetForegroundDrawList();
            Draw(dl, L, c, m, views, f.fonts, Pointer{}, now + i * 16, &hits);
            vtx = dl->VtxBuffer.Size; idx = dl->IdxBuffer.Size;
            ImGui::EndFrame();
        }
        QueryPerformanceCounter(&t1);
        const double us = (t1.QuadPart - t0.QuadPart) * 1e6 / freq.QuadPart / 2000.0;
        Report("    %-14s %7.1f us/frame (incl. ImGui NewFrame/EndFrame)  %5d vtx  %5d idx  %2d hit regions\n",
               k.name, us, vtx, idx, hits.count);
    }
    // Hidden: the integration only calls Update(), which returns immediately.
    Controller hidden;
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    volatile int sink = 0;
    for (int i = 0; i < 1000000; ++i) { hidden.Update(static_cast<uint64_t>(i)); sink += hidden.Visible(); }
    QueryPerformanceCounter(&t1);
    Report("    %-14s %7.4f us/frame (Controller::Update while HIDDEN, no draw)\n", "hidden",
           (t1.QuadPart - t0.QuadPart) * 1e6 / freq.QuadPart / 1000000.0);
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

} // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) { std::puts("usage: SKRPhonePreview <outDir> <backdrop.png> [wallpaper.bmp]"); return 2; }
    g_out = argv[1];
    CreateDirectoryA(g_out.c_str(), nullptr);
    g_report = fopen((g_out + "\\report.txt").c_str(), "w");
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_wic)))) return 3;
    std::vector<uint32_t> bg; UINT bw = 0, bh = 0;
    if (std::string(argv[2])=="--solid") { bg={0xFF17202C}; bw=bh=1; }
    else if (!LoadBgra(argv[2], bg, bw, bh)) { Report("backdrop not loaded: %s\n", argv[2]); return 4; }

    WNDCLASSA wc{}; wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleA(nullptr); wc.lpszClassName = "SKRPhonePreview";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA(wc.lpszClassName, "SKR phone preview", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!hwnd || !d3d) return 5;

    int written = 0, failed = 0;
    const UINT sizes[][2] = {{1280, 720}, {1360, 768}, {1600, 900}, {1920, 1080}, {2560, 1440}, {1024, 768}, {2560, 1080}};
    for (const auto& sz : sizes) {
        D3DPRESENT_PARAMETERS pp{};
        pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferFormat = D3DFMT_UNKNOWN;
        pp.BackBufferWidth = sz[0]; pp.BackBufferHeight = sz[1]; pp.hDeviceWindow = hwnd;
        IDirect3DDevice9* dev = nullptr;
        if (FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev))) {
            Report("CreateDevice %ux%u failed\n", sz[0], sz[1]); ++failed; continue;
        }
        IDirect3DTexture9* tex = nullptr;
        dev->CreateTexture(bw, bh, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr);
        D3DLOCKED_RECT lr{};
        tex->LockRect(0, &lr, nullptr, 0);
        for (UINT y = 0; y < bh; ++y) memcpy(static_cast<BYTE*>(lr.pBits) + y * lr.Pitch, &bg[y * bw], bw * 4);
        tex->UnlockRect(0);

        // ImGui exactly as native-pause Initialize loads its faces (40 px, OS 2x1).
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.Fonts->AddFontDefault();
        static const ImWchar kRanges[] = {0x0020, 0x00FF, 0x0100, 0x017F, 0x0218, 0x021B, 0};
        auto load = [&](const char* file, float px) -> ImFont* {
            char p[MAX_PATH]; char dir[MAX_PATH]; GetWindowsDirectoryA(dir, MAX_PATH);
            snprintf(p, sizeof(p), "%s\\Fonts\\%s", dir, file);
            if (GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES) return nullptr;
            ImFontConfig cfg; cfg.OversampleH = 2; cfg.OversampleV = 1;
            return io.Fonts->AddFontFromFileTTF(p, px, &cfg, kRanges);
        };
        // The pause renderer's 40 px faces plus two phone sizes for this
        // backbuffer height (what the in-game owner would rasterise at init).
        const float k = sz[1] / 768.0f;
        const float pxSizes[3] = {std::round(10.0f * k), std::round(20.0f * k), 40.0f};
        Fonts fonts;
        const char* headingFile = GetFileAttributesA("C:\\Windows\\Fonts\\bahnschrift.ttf") != INVALID_FILE_ATTRIBUTES ? "bahnschrift.ttf" : "arialbd.ttf";
        const char* bodyFile = GetFileAttributesA("C:\\Windows\\Fonts\\segoeui.ttf") != INVALID_FILE_ATTRIBUTES ? "segoeui.ttf" : "arial.ttf";
        for (int i = 0; i < 3; ++i) {
            fonts.heading[i] = load(headingFile, pxSizes[i]);
            fonts.body[i] = load(bodyFile, pxSizes[i]);
        }
        ImGui_ImplDX9_Init(dev);
        if (argc >= 4) {
            wchar_t wp[MAX_PATH];
            MultiByteToWideChar(CP_ACP, 0, argv[3], -1, wp, MAX_PATH);
            Report("wallpaper %s: %s\n", argv[3], skr::phone::wallpaper::LoadFile(dev, wp) ? "loaded" : "NOT loaded (procedural)");
        }
        Frame f{sz[0], sz[1], dev, tex, bw, bh, fonts};

        Report("\n== %ux%u ==\n", sz[0], sz[1]);
        const bool ultrawide = sz[0] * 9 > sz[1] * 16 + 8;
        for (const Scene& sc : kScenes) {
            if (ultrawide && std::string(sc.name) != "03_home" && std::string(sc.name) != "05b_messages_conversation") continue;
            if (RenderScene(f, sc, sc.name, 1.0f, !ultrawide)) ++written; else ++failed;
        }
        if (sz[1] == 1080 && !ultrawide) {
            for (float s : {0.8f, 1.25f}) {
                char tag[64];
                std::snprintf(tag, sizeof(tag), "13_home_uiscale_%03d", static_cast<int>(s * 100));
                if (RenderScene(f, kScenes[3], tag, s, false)) ++written; else ++failed;
            }
        }
        if (!ultrawide) MeasureCost(f);

        skr::phone::wallpaper::Release();
        ImGui_ImplDX9_Shutdown();
        ImGui::DestroyContext();
        tex->Release();
        dev->Release();
    }
    d3d->Release();
    DestroyWindow(hwnd);
    Report("\nwritten %d, failed %d\n", written, failed);
    if (g_report) fclose(g_report);
    g_wic->Release();
    CoUninitialize();
    return failed ? 1 : 0;
}
