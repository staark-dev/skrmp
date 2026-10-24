// API-003.0.3 — GPS route draw must leave D3D9 state exactly as found.
// Uses a real HAL device on a hidden window. Exit 0 = pass, 1 = fail, 2 = skipped.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <cstdio>

#include "gps/gps_draw.h"
#include "gps/gps_flags.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { if (cond) { ++g_pass; } else { ++g_fail; std::printf("  FAIL: %s\n", msg); } } while (0)

using skr::gps::ScreenPoint;
using skr::gps::DrawRouteStrip;

struct Snapshot {
    IDirect3DVertexBuffer9* vb = nullptr; UINT offset = 0, stride = 0;
    IDirect3DVertexDeclaration9* decl = nullptr;
    DWORD fvf = 0;
    IDirect3DBaseTexture9* tex = nullptr;
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DPixelShader9* ps = nullptr;
    RECT scissor{};
    D3DVIEWPORT9 vp{};
    DWORD rs[8]{};
    DWORD tss[6]{};
};

static const D3DRENDERSTATETYPE kRs[8] = {
    D3DRS_SCISSORTESTENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND,
    D3DRS_ZENABLE, D3DRS_CULLMODE, D3DRS_LIGHTING, D3DRS_ALPHATESTENABLE };

static Snapshot Take(IDirect3DDevice9* d) {
    Snapshot s;
    d->GetStreamSource(0, &s.vb, &s.offset, &s.stride);
    d->GetVertexDeclaration(&s.decl);
    d->GetFVF(&s.fvf);
    d->GetTexture(0, &s.tex);
    d->GetVertexShader(&s.vs);
    d->GetPixelShader(&s.ps);
    d->GetScissorRect(&s.scissor);
    d->GetViewport(&s.vp);
    for (int i = 0; i < 8; ++i) d->GetRenderState(kRs[i], &s.rs[i]);
    d->GetTextureStageState(0, D3DTSS_COLOROP, &s.tss[0]);
    d->GetTextureStageState(0, D3DTSS_COLORARG1, &s.tss[1]);
    d->GetTextureStageState(0, D3DTSS_ALPHAOP, &s.tss[2]);
    d->GetTextureStageState(0, D3DTSS_ALPHAARG1, &s.tss[3]);
    d->GetTextureStageState(1, D3DTSS_COLOROP, &s.tss[4]);
    d->GetTextureStageState(1, D3DTSS_ALPHAOP, &s.tss[5]);
    // Get* AddRef'd; the device still holds its own references.
    if (s.vb) s.vb->Release();
    if (s.decl) s.decl->Release();
    if (s.tex) s.tex->Release();
    if (s.vs) s.vs->Release();
    if (s.ps) s.ps->Release();
    return s;
}

static bool Same(const Snapshot& a, const Snapshot& b, const char* when) {
    bool ok = true;
    auto chk = [&](bool c, const char* what) { if (!c) { std::printf("  [%s] changed: %s\n", when, what); ok = false; } };
    chk(a.vb == b.vb && a.offset == b.offset && a.stride == b.stride, "stream 0 source/offset/stride");
    chk(a.decl == b.decl, "vertex declaration");
    chk(a.fvf == b.fvf, "FVF");
    chk(a.tex == b.tex, "texture 0");
    chk(a.vs == b.vs && a.ps == b.ps, "shaders");
    chk(EqualRect(&a.scissor, &b.scissor) != FALSE, "scissor rect");
    chk(a.vp.X == b.vp.X && a.vp.Y == b.vp.Y && a.vp.Width == b.vp.Width && a.vp.Height == b.vp.Height, "viewport");
    for (int i = 0; i < 8; ++i) if (a.rs[i] != b.rs[i]) { chk(false, "render state"); break; }
    for (int i = 0; i < 6; ++i) if (a.tss[i] != b.tss[i]) { chk(false, "texture stage state"); break; }
    return ok;
}

static void TestFlags() {
    std::printf("== Feature switch transitions ==\n");
    using skr::gps::flags::Flags;
    using skr::gps::flags::Diff;
    Flags off{}, core{true, false, false}, all{true, true, true}, rend{false, true, false};
    CHECK(!Diff(off, off).resetCore && !Diff(off, off).resetRenderer, "off->off: nothing");
    CHECK(!Diff(off, all).resetCore && !Diff(off, all).resetRenderer, "enabling never resets");
    CHECK(Diff(all, off).resetCore && Diff(all, off).resetRenderer, "all->off: core + renderer reset");
    CHECK(Diff(core, off).resetCore && Diff(core, off).resetRenderer, "core off also drops renderer cache");
    CHECK(!Diff(all, core).resetCore && Diff(all, core).resetRenderer, "renderer off: renderer reset only");
    CHECK(!Diff(rend, off).resetCore && Diff(rend, off).resetRenderer, "renderer-only off");
    Flags s = off;
    int resets = 0;
    for (int i = 0; i < 50; ++i) { Flags n = (i % 2) ? off : all; resets += Diff(s, n).resetCore ? 1 : 0; s = n; }
    CHECK(resets == 25, "50 toggles -> 25 core resets");
}

int main() {
    TestFlags();

    std::printf("== D3D9 state restoration (real device) ==\n");
    WNDCLASSA wc{}; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(nullptr); wc.lpszClassName = "SKRGpsStateTest";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA(wc.lpszClassName, "", WS_OVERLAPPEDWINDOW, 0, 0, 640, 480, nullptr, nullptr, wc.hInstance, nullptr);
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    IDirect3DDevice9* dev = nullptr;
    D3DPRESENT_PARAMETERS pp{}; pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = hwnd;
    pp.BackBufferFormat = D3DFMT_UNKNOWN; pp.BackBufferWidth = 640; pp.BackBufferHeight = 480;
    if (!d3d || FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                         D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev)) || !dev) {
        std::printf("SKIPPED: no HAL D3D9 device available\n");
        return 2;
    }

    // RenderWare-like state: bound VB + declaration, texture, scissor, blends.
    IDirect3DVertexBuffer9* vb = nullptr;
    dev->CreateVertexBuffer(24 * 64, D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &vb, nullptr);
    const D3DVERTEXELEMENT9 elems[] = {
        {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITIONT, 0},
        {0, 16, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {0, 20, D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END() };
    IDirect3DVertexDeclaration9* decl = nullptr;
    dev->CreateVertexDeclaration(elems, &decl);
    IDirect3DTexture9* tex = nullptr;
    dev->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr);
    CHECK(vb && decl && tex, "test resources created");

    dev->SetStreamSource(0, vb, 0, 24);
    dev->SetVertexDeclaration(decl);
    dev->SetTexture(0, tex);
    RECT sc{10, 20, 300, 200};
    dev->SetScissorRect(&sc);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ZERO);
    dev->SetRenderState(D3DRS_ZENABLE, TRUE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);

    const ScreenPoint pts[] = {{50, 50}, {120, 80}, {200, 60}, {260, 150}};
    const RECT clip{40, 40, 280, 180};

    dev->BeginScene();
    const Snapshot before = Take(dev);
    CHECK(before.vb == vb && before.stride == 24, "baseline stream 0 bound");

    CHECK(DrawRouteStrip(dev, pts, 4, &clip), "draw with clip succeeds");
    CHECK(Same(before, Take(dev), "with clip"), "state restored after clipped draw");

    CHECK(DrawRouteStrip(dev, pts, 4, nullptr), "draw without clip succeeds");
    CHECK(Same(before, Take(dev), "no clip"), "state restored after unclipped draw");

    bool loopOk = true;
    for (int i = 0; i < 200 && loopOk; ++i)
        loopOk = DrawRouteStrip(dev, pts, 4, (i & 1) ? &clip : nullptr) && Same(before, Take(dev), "loop");
    CHECK(loopOk, "200 repeated draws keep state");

    CHECK(!DrawRouteStrip(dev, pts, 1, &clip), "single point rejected");
    CHECK(!DrawRouteStrip(dev, nullptr, 4, &clip), "null points rejected");
    CHECK(!DrawRouteStrip(nullptr, pts, 4, &clip), "null device rejected");
    CHECK(Same(before, Take(dev), "rejected"), "rejected calls change nothing");

    // Root-cause mechanism: an unwrapped DrawPrimitiveUP unbinds stream 0 and
    // replaces the declaration, while RenderWare's cache still holds the old ones.
    struct V { float x, y, z, w; DWORD c; } tri[3] = {{0,0,0,1,0}, {10,0,0,1,0}, {0,10,0,1,0}};
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, sizeof(V));
    IDirect3DVertexBuffer9* after = nullptr; UINT off = 0, str = 0;
    dev->GetStreamSource(0, &after, &off, &str);
    IDirect3DVertexDeclaration9* afterDecl = nullptr;
    dev->GetVertexDeclaration(&afterDecl);
    CHECK(after == nullptr, "raw DrawPrimitiveUP unbinds stream 0 (bug mechanism)");
    CHECK(afterDecl != decl, "raw SetFVF replaces the declaration (bug mechanism)");
    if (after) after->Release();
    if (afterDecl) afterDecl->Release();
    dev->EndScene();

    tex->Release(); decl->Release(); vb->Release();
    dev->Release(); d3d->Release();
    DestroyWindow(hwnd);

    std::printf("\ngps render state: %d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
