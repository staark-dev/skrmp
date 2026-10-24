#include "game-state/game_state.h"
#include "intro/launch_cover.h"
#include "lobby/lobby_manager.h"
#include "intro/launch_signals.h"
#include "ipc/handoff_trace.h"
#include <atomic>
#include <cstring>
namespace skr::launch_cover {
namespace {
HANDLE frameEvent{}, takeoverEvent{};
bool finished = false, completionPending = false, frameReported = false;
bool completeOnPresent = false;   // completion frame drawn without the cover
bool worldTraced = false, takeoverTraced = false, netGameTraced = false;
std::atomic<bool> gameplayConfirmed{false};
std::atomic<bool> netGameReady{false};
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
void Configure(uint32_t frame, uint32_t takeover) {
    frameEvent = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(frame));
    takeoverEvent = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(takeover));
}
void BeforeReset() { if (title) { title->Release(); title = nullptr; } }
bool Draw(IDirect3DDevice9* device, bool worldConnected) {
    if (!frameEvent || finished) return false;
    // Bootstrap runs even while the loading cover is active.
    skr::lobby::OnEndSceneAfterIntro(device);

    // Cover dismisses at the first frame where BOTH hold:
    //   1. CNetGame exists (netGameReady, set from game_state::Tick)
    //      — this fires BEFORE SA-MP renders the "Started" chat message.
    //      Fallbacks: worldConnected (state 5) or gameplayConfirmed (5s failsafe).
    //   2. Launcher takeover event is signalled (no desktop flash).
    const bool ready = netGameReady.load(std::memory_order_relaxed);
    const bool confirmed = gameplayConfirmed.load(std::memory_order_relaxed);
    const bool takeover = WaitForSingleObject(takeoverEvent, 0) == WAIT_OBJECT_0;

    completionPending = (ready || confirmed || worldConnected)
                     && takeover;

    // One-shot timing traces.
    if (ready && !netGameTraced) {
        netGameTraced = true;
        char buf[80]; wsprintfA(buf, "COVER-TIMING: netGameReady at %lu", GetTickCount());
        ipc::TraceHandoff(buf);
    }
    if (worldConnected && !worldTraced) {
        worldTraced = true;
        char buf[80]; wsprintfA(buf, "COVER-TIMING: state5 at %lu", GetTickCount());
        ipc::TraceHandoff(buf);
    }
    if (takeover && !takeoverTraced) {
        takeoverTraced = true;
        char buf[80]; wsprintfA(buf, "COVER-TIMING: takeover at %lu", GetTickCount());
        ipc::TraceHandoff(buf);
    }
    // Completion frame: present the game, not the cover. Visual-ready is
    // signalled in Presented() only after this frame is actually presented.
    if (completionPending) {
        completeOnPresent = true;
        return false;
    }
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
    if (result != D3D_OK) {
        if (completeOnPresent && !finished) {
            char buf[96]; wsprintfA(buf, "COVER: completion frame Present failed hr=0x%08lX, retrying", result);
            ipc::TraceHandoff(buf);
            completeOnPresent = false;   // Draw() re-arms it on the next frame
        }
        return;
    }
    if (drewCover) {
        SetEvent(frameEvent);
        if (!frameReported) { frameReported = true; ipc::TraceHandoff("cover-presented"); }
    }
    if (completeOnPresent && !finished) {
        finished = true;
        completeOnPresent = false;
        char buf[80];
        wsprintfA(buf, "COVER-TIMING: dismissed at %lu", GetTickCount());
        ipc::TraceHandoff(buf);
        launch_signals::Signal(ipc::Milestone::VisualReady, "after presented frame");
        BeforeReset();
    }
}
void MarkGameplayConfirmed() {
    if (gameplayConfirmed.exchange(true, std::memory_order_relaxed)) return;
    ipc::TraceHandoff("LAUNCH-COVER: gameplayConfirmed = true");
    launch_signals::Signal(ipc::Milestone::GameplayConfirmed, "in world, independent of Present");
}
void MarkNetGameReady() {
    if (netGameReady.exchange(true, std::memory_order_relaxed)) return;
    ipc::TraceHandoff("LAUNCH-COVER: netGameReady = true (CNetGame exists)");
    launch_signals::Signal(ipc::Milestone::SampReady, "CNetGame created");
}
}
