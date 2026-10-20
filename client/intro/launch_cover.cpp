#include "game-state/game_state.h"
#include "intro/launch_cover.h"
#include "lobby/lobby_manager.h"
#include "cef-local/cef_local_loader.h"
#include "ipc/handoff_trace.h"
#include <cstring>
namespace skr::launch_cover {
namespace {
HANDLE frameEvent{}, takeoverEvent{}, readyEvent{};
bool finished = false, completionPending = false, frameReported = false;
IDirect3DTexture9* title = nullptr;
constexpr int width = 640, height = 200;
bool CreateTitle(IDirect3DDevice9* device) {
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
    void* pixels = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!dc || !bitmap || !pixels) { if (bitmap) DeleteObject(bitmap); if (dc) DeleteDC(dc); return false; }
    auto oldBitmap = SelectObject(dc, bitmap);
    RECT bounds{0, 0, width, height};
    HBRUSH background = CreateSolidBrush(RGB(9, 10, 10));
    FillRect(dc, &bounds, background); DeleteObject(background);
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(117, 186, 82));
    HFONT font = CreateFontW(-64, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    auto oldFont = SelectObject(dc, font);
    RECT text{0, 30, width, 110}; DrawTextW(dc, L"SKR CLIENT", -1, &text, DT_CENTER | DT_SINGLELINE);
    SelectObject(dc, oldFont); DeleteObject(font);
    font = CreateFontW(-18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    SelectObject(dc, font); SetTextColor(dc, RGB(221, 226, 226));
    text = {0, 125, width, 160}; DrawTextW(dc, L"Preparing your session...", -1, &text, DT_CENTER | DT_SINGLELINE);
    GdiFlush();
    bool ok = SUCCEEDED(device->CreateTexture(width, height, 1, 0, D3DFMT_X8R8G8B8, D3DPOOL_MANAGED, &title, nullptr));
    D3DLOCKED_RECT locked{};
    if (ok) {
        ok = SUCCEEDED(title->LockRect(0, &locked, nullptr, 0));
        if (ok) {
            for (int y = 0; y < height; ++y)
                memcpy(static_cast<char*>(locked.pBits) + y * locked.Pitch, static_cast<char*>(pixels) + y * width * 4, width * 4);
            title->UnlockRect(0);
        }
    }
    SelectObject(dc, oldFont); DeleteObject(font); SelectObject(dc, oldBitmap);
    DeleteObject(bitmap); DeleteDC(dc);
    if (!ok && title) { title->Release(); title = nullptr; }
    return ok;
}
}
void Configure(uint32_t frame, uint32_t takeover, uint32_t ready) {
    frameEvent = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(frame));
    takeoverEvent = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(takeover));
    readyEvent = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(ready));
}
void BeforeReset() { if (title) { title->Release(); title = nullptr; } }
bool Draw(IDirect3DDevice9* device, bool worldConnected) {
    if (!frameEvent || finished) return false;
    // Bootstrap runs even while the loading cover is active.
    skr::lobby::OnEndSceneAfterIntro(device);
    using ReadyFn = bool(__cdecl*)();
    auto module = GetModuleHandleW(L"client.dll");
    auto ready = module ? reinterpret_cast<ReadyFn>(GetProcAddress(module, "skr_cef_session_visual_ready_v1")) : nullptr;
    const bool visualReady = skr::lobby::IsSkrSession() ? (ready && ready())
        : !skr::game_state::IsEnhancedNegotiationPending();
    completionPending = worldConnected && visualReady &&
                        WaitForSingleObject(takeoverEvent, 0) == WAIT_OBJECT_0;
    if (FAILED(device->TestCooperativeLevel())) return false;
    if (!title && !CreateTitle(device)) return false;
    IDirect3DStateBlock9* saved = nullptr;
    if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &saved))) return false;
    if (FAILED(saved->Capture())) { saved->Release(); return false; }
    bool ok = SUCCEEDED(device->BeginScene());
    if (ok) {
        D3DVIEWPORT9 vp{}; device->GetViewport(&vp);
        ok = SUCCEEDED(device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(9, 10, 10), 1, 0));
        const float x = (static_cast<float>(vp.Width) - width) / 2, y = (static_cast<float>(vp.Height) - height) / 2;
        struct Vertex { float x,y,z,w,u,v; };
        Vertex vertices[] = {{x-.5f,y-.5f,0,1,0,0},{x+width-.5f,y-.5f,0,1,1,0},
                             {x-.5f,y+height-.5f,0,1,0,1},{x+width-.5f,y+height-.5f,0,1,1,1}};
        device->SetVertexShader(nullptr); device->SetPixelShader(nullptr);
        device->SetRenderState(D3DRS_ZENABLE, FALSE); device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE); device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, 15);
        device->SetTexture(0, title); device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        ok = SUCCEEDED(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(Vertex))) && ok;
        ok = SUCCEEDED(device->EndScene()) && ok;
    }
    saved->Apply(); saved->Release();
    return ok;
}
void Presented(bool drewCover, HRESULT result) {
    if (!drewCover || result != D3D_OK) return;
    SetEvent(frameEvent);
    if (!frameReported) { frameReported = true; ipc::TraceHandoff("cover-presented"); }
    if (completionPending) { finished = true; SetEvent(readyEvent); ipc::TraceHandoff("world-resources-hud-ready"); BeforeReset(); }
}
}
