#include "render-d3d9/chat_box.h"
#include <windows.h>
#include <d3d9.h>
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>

// Provenance: samp.dll 0.3.7-R5 (sha256 b72b5dbe...2553a2), disassembled locally and
// matched with D:\SA-MP Reverse\RE\client\research\docs\subsystems\chat.md:
//   RVA 0x75A81            call Chat_Draw (0x67E00) in D3D9Proxy_Present; __thiscall(CChat*)
//   RVA 0x67EB7, 0x67FCD   call Chat_Render (0x67940); __thiscall(CChat*), plain ret
//   RVA 0x67A7A, 0x67ADB, 0x67B44, 0x67B7B
//                          call Chat_DrawTextLine (0x67470); __thiscall(CChat*, text,
//                          left, top, right, bottom, colour), ret 0x18
//   Chat_DrawTextLine draws the main text with ID3DXFont [[CChat+0x63A2]+0] via
//   DrawTextA (vtable +0x38) and DT_NOCLIP|DT_SINGLELINE.
//   CChat: +0x8 m_iDisplayMode, +0x63AE IDirect3DDevice9*, +0x63E2 m_iLineHeight.
//   Chat_Render starts text at x=45, y=10.
// GTA SA 1.0 US (plugin-sdk): FrontEndMenuManager 0xBA6748, m_bMenuActive @0x5C.

namespace skr::render {

namespace {

constexpr uintptr_t kChatDrawCall = 0x75A81, kChatDraw = 0x67E00;
constexpr uintptr_t kChatRender = 0x67940;
constexpr uintptr_t kChatRenderCalls[] = {0x67EB7, 0x67FCD};
constexpr uintptr_t kDrawLine = 0x67470;
constexpr uintptr_t kDrawLineCalls[] = {0x67A7A, 0x67ADB, 0x67B44, 0x67B7B};
constexpr uintptr_t kMenuActive = 0xBA6748 + 0x5C;

constexpr int kTextLeft = 45;
constexpr int kPadX = 10, kPadY = 6;
constexpr float kRadius = 8.0f;
constexpr DWORD kPanelColor = D3DCOLOR_ARGB(140, 18, 23, 30);  // rgba(18,23,30,.55)
constexpr float kPanelWidth = 394.0f;
constexpr float kPanelHeight = 113.0f;

using ChatFn = void(__thiscall*)(void*);
using DrawLineFn = void(__thiscall*)(void*, const char*, int, int, int, int, DWORD);
using FontDrawTextFn = int(__stdcall*)(void* font, void* sprite, const char* text, int count,
                                       RECT* rect, DWORD format, DWORD color);

ChatFn g_origChatDraw = nullptr;
ChatFn g_origChatRender = nullptr;
DrawLineFn g_origDrawLine = nullptr;

// Content extents measured during the last Chat_Render pass.
// Chat fills from the bottom, so the top edge is the first non-empty line, not kTextTop.
int g_passTop = INT_MAX, g_passRight = 0, g_passBottom = 0;
int g_contentTop = INT_MAX, g_contentRight = 0, g_contentBottom = 0;

int MeasureText(uint8_t* chat, const char* text) {
    auto* fontManager = *reinterpret_cast<uint8_t**>(chat + 0x63A2);
    if (!fontManager) return 0;
    void* font = *reinterpret_cast<void**>(fontManager);
    if (!font) return 0;

    // Drop {RRGGBB} colour codes; they are not drawn.
    char buf[256];
    size_t n = 0;
    for (const char* p = text; *p && n < sizeof(buf) - 1; ++p) {
        if (p[0] == '{' && strnlen(p, 8) >= 8 && p[7] == '}') { p += 7; continue; }
        buf[n++] = *p;
    }
    buf[n] = '\0';
    if (n == 0) return 0;

    RECT rc{0, 0, 0, 0};
    auto vtbl = *reinterpret_cast<void***>(font);
    auto drawText = reinterpret_cast<FontDrawTextFn>(vtbl[0x38 / 4]);
    drawText(font, nullptr, buf, -1, &rc, DT_CALCRECT | DT_SINGLELINE | DT_NOCLIP, 0);
    return rc.right - rc.left;
}

void __fastcall HookedDrawLine(void* chat, void*, const char* text,
                               int left, int top, int right, int bottom, DWORD colour) {
    // Clip lines that would extend above the fixed-height panel. Uses the previous
    // frame's content bottom as the anchor so the cutoff tracks newly added lines.
    if (g_contentBottom > 0 && top < g_contentBottom + kPadY - static_cast<int>(kPanelHeight)) {
        if (text && *text) {
            int lineHeight = *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(chat) + 0x63E2);
            int w = MeasureText(static_cast<uint8_t*>(chat), text);
            if (w > 0) {
                g_passRight = (std::max)(g_passRight, left + w);
                g_passBottom = (std::max)(g_passBottom, top + lineHeight);
                g_passTop = (std::min)(g_passTop, top);
            }
        }
        return;
    }
    g_origDrawLine(chat, text, left, top, right, bottom, colour);
    if (!text || !*text) return;
    int w = MeasureText(static_cast<uint8_t*>(chat), text);
    if (w <= 0) return;
    int lineHeight = *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(chat) + 0x63E2);
    g_passTop = (std::min)(g_passTop, top);
    g_passRight = (std::max)(g_passRight, left + w);
    g_passBottom = (std::max)(g_passBottom, top + lineHeight);
}

void __fastcall HookedChatRender(void* chat, void*) {
    g_passTop = INT_MAX;
    g_passRight = g_passBottom = 0;
    g_origChatRender(chat);
    g_contentTop = g_passTop;
    g_contentRight = g_passRight;
    g_contentBottom = g_passBottom;
}

struct Vertex { float x, y, z, rhw; DWORD color; };

void DrawRoundedRect(IDirect3DDevice9* dev, float l, float t, float r, float b, DWORD color) {
    constexpr int kSeg = 6;
    Vertex v[2 + 4 * (kSeg + 1)];
    int n = 0;
    v[n++] = {(l + r) / 2, (t + b) / 2, 0, 1, color};
    const float cx[4] = {r - kRadius, l + kRadius, l + kRadius, r - kRadius};
    const float cy[4] = {t + kRadius, t + kRadius, b - kRadius, b - kRadius};
    for (int c = 0; c < 4; ++c) {
        for (int s = 0; s <= kSeg; ++s) {
            float a = (c * 90.0f + s * 90.0f / kSeg) * 3.14159265f / 180.0f;
            v[n++] = {cx[c] + std::cos(a) * kRadius, cy[c] - std::sin(a) * kRadius, 0, 1, color};
        }
    }
    v[n++] = v[1];

    DWORD ab, sb, db, lt, ze, cl, fvf, cop, ca1, aop, aa1;
    IDirect3DBaseTexture9* tex = nullptr;
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DPixelShader9* ps = nullptr;
    dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &ab);
    dev->GetRenderState(D3DRS_SRCBLEND, &sb);
    dev->GetRenderState(D3DRS_DESTBLEND, &db);
    dev->GetRenderState(D3DRS_LIGHTING, &lt);
    dev->GetRenderState(D3DRS_ZENABLE, &ze);
    dev->GetRenderState(D3DRS_CULLMODE, &cl);
    dev->GetTextureStageState(0, D3DTSS_COLOROP, &cop);
    dev->GetTextureStageState(0, D3DTSS_COLORARG1, &ca1);
    dev->GetTextureStageState(0, D3DTSS_ALPHAOP, &aop);
    dev->GetTextureStageState(0, D3DTSS_ALPHAARG1, &aa1);
    dev->GetTexture(0, &tex);
    dev->GetFVF(&fvf);
    dev->GetVertexShader(&vs);
    dev->GetPixelShader(&ps);

    dev->SetVertexShader(nullptr);
    dev->SetPixelShader(nullptr);
    dev->SetTexture(0, nullptr);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, n - 2, v, sizeof(Vertex));

    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, ab);
    dev->SetRenderState(D3DRS_SRCBLEND, sb);
    dev->SetRenderState(D3DRS_DESTBLEND, db);
    dev->SetRenderState(D3DRS_LIGHTING, lt);
    dev->SetRenderState(D3DRS_ZENABLE, ze);
    dev->SetRenderState(D3DRS_CULLMODE, cl);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, cop);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, ca1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, aop);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, aa1);
    dev->SetTexture(0, tex);
    if (tex) tex->Release();
    dev->SetFVF(fvf);
    dev->SetVertexShader(vs);
    if (vs) vs->Release();
    dev->SetPixelShader(ps);
    if (ps) ps->Release();
}

void __fastcall HookedChatDraw(void* chatPtr, void*) {
    auto* chat = static_cast<uint8_t*>(chatPtr);
    auto* dev = *reinterpret_cast<IDirect3DDevice9**>(chat + 0x63AE);
    uint32_t mode = *reinterpret_cast<uint32_t*>(chat + 0x8);
    if (dev && mode != 0 && !*reinterpret_cast<bool*>(kMenuActive) &&
        g_contentRight > kTextLeft && g_contentBottom > g_contentTop) {
        // Chat fills from the bottom, so anchor the fixed-size panel at the last line.
        float l = float(kTextLeft - kPadX);
        float b = float(g_contentBottom + kPadY);
        DrawRoundedRect(dev, l, b - kPanelHeight, l + kPanelWidth, b, kPanelColor);
    }
    g_origChatDraw(chatPtr);
}

// Points the rel32 of an E8 call at `hook` if it currently targets base+expectRva.
bool RedirectCall(uintptr_t base, uintptr_t siteRva, uintptr_t expectRva, void* hook) {
    auto* site = reinterpret_cast<uint8_t*>(base + siteRva);
    if (site[0] != 0xE8) return false;
    if (base + siteRva + 5 + *reinterpret_cast<int32_t*>(site + 1) != base + expectRva) return false;
    int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(hook) - (base + siteRva + 5));
    DWORD old = 0;
    if (!VirtualProtect(site + 1, 4, PAGE_EXECUTE_READWRITE, &old)) return false;
    *reinterpret_cast<int32_t*>(site + 1) = rel;
    VirtualProtect(site + 1, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    return true;
}

bool SiteMatches(uintptr_t base, uintptr_t siteRva, uintptr_t expectRva) {
    auto* site = reinterpret_cast<uint8_t*>(base + siteRva);
    return site[0] == 0xE8 &&
           base + siteRva + 5 + *reinterpret_cast<int32_t*>(site + 1) == base + expectRva;
}

} // namespace

bool InstallChatBoxHook() {
    HMODULE samp = GetModuleHandleA("samp.dll");
    if (!samp) return false;
    auto base = reinterpret_cast<uintptr_t>(samp);

    // All-or-nothing: patch only if every site still has its original target.
    if (!SiteMatches(base, kChatDrawCall, kChatDraw)) return false;
    for (auto s : kChatRenderCalls) if (!SiteMatches(base, s, kChatRender)) return false;
    for (auto s : kDrawLineCalls) if (!SiteMatches(base, s, kDrawLine)) return false;

    g_origChatDraw = reinterpret_cast<ChatFn>(base + kChatDraw);
    g_origChatRender = reinterpret_cast<ChatFn>(base + kChatRender);
    g_origDrawLine = reinterpret_cast<DrawLineFn>(base + kDrawLine);

    for (auto s : kDrawLineCalls) RedirectCall(base, s, kDrawLine, reinterpret_cast<void*>(&HookedDrawLine));
    for (auto s : kChatRenderCalls) RedirectCall(base, s, kChatRender, reinterpret_cast<void*>(&HookedChatRender));
    return RedirectCall(base, kChatDrawCall, kChatDraw, reinterpret_cast<void*>(&HookedChatDraw));
}

} // namespace skr::render
