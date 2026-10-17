#include "render-d3d9/intro_renderer.h"
#include <algorithm>
#include <cstring>

namespace skr::render {

namespace {

struct Vertex {
    float x, y, z, rhw;
    DWORD color;
    float u, v;
};
constexpr DWORD kVertexFVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;

constexpr int kLogoWidth  = 256;
constexpr int kLogoHeight = 128;

void RenderLetterS(uint32_t* pixels, int px, int py, uint32_t col) {
    // Simplified block-letter "S" on a 24x32 grid
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 24; ++x) {
            bool on = false;
            if (y < 6)               on = (x >= 2 && x < 22);       // top bar
            else if (y < 14)         on = (x >= 2 && x < 8);        // left side
            else if (y < 20)         on = (x >= 2 && x < 22);       // middle bar
            else if (y < 26)         on = (x >= 16 && x < 22);      // right side
            else                     on = (x >= 2 && x < 22);       // bottom bar
            if (on) pixels[(py + y) * kLogoWidth + (px + x)] = col;
        }
}

void RenderLetterK(uint32_t* pixels, int px, int py, uint32_t col) {
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 24; ++x) {
            bool on = (x >= 2 && x < 8); // vertical bar
            int diag = y - 16;
            int dx = (diag < 0) ? (-diag * 24 / 16) : (diag * 24 / 16);
            if (x >= 8 && x < 22 && abs(x - 8 - dx) < 4) on = true;
            if (on) pixels[(py + y) * kLogoWidth + (px + x)] = col;
        }
}

void RenderLetterR(uint32_t* pixels, int px, int py, uint32_t col) {
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 24; ++x) {
            bool on = (x >= 2 && x < 8); // vertical bar
            if (y < 6 && x >= 2 && x < 20) on = true;              // top bar
            if (y >= 6 && y < 14 && x >= 16 && x < 22) on = true;  // top-right
            if (y >= 14 && y < 18 && x >= 2 && x < 20) on = true;  // middle bar
            if (y >= 18) {
                int diag = (y - 18) * 14 / 14;
                if (x >= 10 + diag - 2 && x < 10 + diag + 4) on = true; // leg
            }
            if (on) pixels[(py + y) * kLogoWidth + (px + x)] = col;
        }
}

} // anonymous namespace

bool IntroRenderer::CreateLogoTexture(IDirect3DDevice9* device) {
    HRESULT hr = device->CreateTexture(
        kLogoWidth, kLogoHeight, 1, 0,
        D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &logoTexture_, nullptr);
    if (FAILED(hr)) return false;

    D3DLOCKED_RECT locked{};
    hr = logoTexture_->LockRect(0, &locked, nullptr, 0);
    if (FAILED(hr)) { logoTexture_->Release(); logoTexture_ = nullptr; return false; }

    auto* pixels = static_cast<uint32_t*>(locked.pBits);
    int pitch = locked.Pitch / 4;

    // Clear to transparent
    for (int y = 0; y < kLogoHeight; ++y)
        for (int x = 0; x < kLogoWidth; ++x)
            pixels[y * pitch + x] = 0x00000000;

    // Use pitch-aware pointer wrapper — letters render into contiguous kLogoWidth row
    // Since pitch may differ, render into a temp buffer then copy
    auto* temp = new uint32_t[kLogoWidth * kLogoHeight]();
    int startX = (kLogoWidth - 3 * 24 - 2 * 12) / 2;
    int startY = (kLogoHeight - 32) / 2;
    RenderLetterS(temp, startX, startY, config_.logoColor);
    RenderLetterK(temp, startX + 36, startY, config_.logoColor);
    RenderLetterR(temp, startX + 72, startY, config_.logoColor);

    for (int y = 0; y < kLogoHeight; ++y)
        memcpy(pixels + y * pitch, temp + y * kLogoWidth, kLogoWidth * 4);
    delete[] temp;

    logoTexture_->UnlockRect(0);
    return true;
}

bool IntroRenderer::CreateResources(IDirect3DDevice9* device) {
    if (!device) return false;
    if (!CreateLogoTexture(device)) return false;
    resourcesValid_ = true;
    phase_ = IntroPhase::FadeIn;
    elapsed_ = 0.0f;
    skipRequested_ = false;
    return true;
}

void IntroRenderer::Skip() {
    skipRequested_ = true;
}

void IntroRenderer::DrawOverlay(IDirect3DDevice9* device, float alpha) {
    if (!resourcesValid_ || !logoTexture_) return;

    alpha = (std::max)(0.0f, (std::min)(1.0f, alpha));
    DWORD a = static_cast<DWORD>(alpha * 255.0f);
    DWORD bgA = a;
    DWORD logoA = a;

    uint32_t bgRGB = config_.bgColor & 0x00FFFFFF;

    D3DVIEWPORT9 vp{};
    device->GetViewport(&vp);
    float sw = static_cast<float>(vp.Width);
    float sh = static_cast<float>(vp.Height);

    // Save state
    device->CreateStateBlock(D3DSBT_ALL, &savedState_);
    if (savedState_) savedState_->Capture();

    // Set render state for alpha-blended overlay
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    device->SetFVF(kVertexFVF);

    // Background quad (fullscreen, semi-transparent)
    DWORD bgColor = (bgA << 24) | bgRGB;
    Vertex bgQuad[4] = {
        {0.0f, 0.0f,  0.0f, 1.0f, bgColor, 0.0f, 0.0f},
        {sw,   0.0f,  0.0f, 1.0f, bgColor, 1.0f, 0.0f},
        {0.0f, sh,    0.0f, 1.0f, bgColor, 0.0f, 1.0f},
        {sw,   sh,    0.0f, 1.0f, bgColor, 1.0f, 1.0f},
    };
    device->SetTexture(0, nullptr);
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, bgQuad, sizeof(Vertex));

    // Logo quad (centered)
    float logoW = static_cast<float>(kLogoWidth) * 2.0f;
    float logoH = static_cast<float>(kLogoHeight) * 2.0f;
    float lx = (sw - logoW) * 0.5f;
    float ly = (sh - logoH) * 0.5f;
    // Use white diffuse, texture provides color
    DWORD white = (logoA << 24) | 0x00FFFFFF;
    Vertex logoQuad[4] = {
        {lx,         ly,          0.0f, 1.0f, white, 0.0f, 0.0f},
        {lx + logoW, ly,          0.0f, 1.0f, white, 1.0f, 0.0f},
        {lx,         ly + logoH,  0.0f, 1.0f, white, 0.0f, 1.0f},
        {lx + logoW, ly + logoH,  0.0f, 1.0f, white, 1.0f, 1.0f},
    };
    device->SetTexture(0, logoTexture_);
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, logoQuad, sizeof(Vertex));

    // Restore state
    if (savedState_) {
        savedState_->Apply();
        savedState_->Release();
        savedState_ = nullptr;
    }
}

void IntroRenderer::Draw(IDirect3DDevice9* device, float deltaSeconds) {
    if (phase_ == IntroPhase::Done || !resourcesValid_) return;

    elapsed_ += deltaSeconds;

    if (skipRequested_ && phase_ != IntroPhase::FadeOut) {
        phase_ = IntroPhase::FadeOut;
        elapsed_ = 0.0f;
    }

    float alpha = 0.0f;
    switch (phase_) {
        case IntroPhase::FadeIn:
            alpha = elapsed_ / config_.fadeInSeconds;
            if (elapsed_ >= config_.fadeInSeconds) {
                phase_ = IntroPhase::Display;
                elapsed_ = 0.0f;
                alpha = 1.0f;
            }
            break;
        case IntroPhase::Display:
            alpha = 1.0f;
            if (elapsed_ >= config_.displaySeconds) {
                phase_ = IntroPhase::FadeOut;
                elapsed_ = 0.0f;
            }
            break;
        case IntroPhase::FadeOut:
            alpha = 1.0f - (elapsed_ / config_.fadeOutSeconds);
            if (elapsed_ >= config_.fadeOutSeconds) {
                phase_ = IntroPhase::Done;
                alpha = 0.0f;
            }
            break;
        case IntroPhase::Done:
            return;
    }

    DrawOverlay(device, alpha);
}

void IntroRenderer::BeforeReset() {
    // Logo uses D3DPOOL_MANAGED — survives reset. Nothing to release.
    // State block is transient per-frame, not held across resets.
    resourcesValid_ = false;
}

bool IntroRenderer::AfterReset(IDirect3DDevice9* device) {
    if (!device) return false;
    // D3DPOOL_MANAGED textures are restored automatically.
    resourcesValid_ = (logoTexture_ != nullptr);
    return resourcesValid_;
}

void IntroRenderer::Shutdown() {
    if (savedState_) { savedState_->Release(); savedState_ = nullptr; }
    if (logoTexture_) { logoTexture_->Release(); logoTexture_ = nullptr; }
    resourcesValid_ = false;
    phase_ = IntroPhase::Done;
}

} // namespace skr::render
