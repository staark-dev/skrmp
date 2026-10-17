#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <cstdint>

namespace skr::render {

enum class IntroPhase : uint8_t {
    FadeIn,
    Display,
    FadeOut,
    Done
};

struct IntroConfig {
    float fadeInSeconds  = 0.8f;
    float displaySeconds = 1.5f;
    float fadeOutSeconds = 0.7f;
    uint32_t logoColor   = 0xFFFFFFFF;
    uint32_t bgColor     = 0xFF0A0A0A;
};

class IntroRenderer {
public:
    bool CreateResources(IDirect3DDevice9* device);
    void Draw(IDirect3DDevice9* device, float deltaSeconds);
    void Skip();
    void BeforeReset();
    bool AfterReset(IDirect3DDevice9* device);
    void Shutdown();

    bool IsDone() const { return phase_ == IntroPhase::Done; }
    void SetConfig(const IntroConfig& cfg) { config_ = cfg; }

private:
    void DrawOverlay(IDirect3DDevice9* device, float alpha);
    bool CreateLogoTexture(IDirect3DDevice9* device);

    IDirect3DTexture9* logoTexture_ = nullptr;
    IDirect3DStateBlock9* savedState_ = nullptr;
    IntroConfig config_;
    IntroPhase phase_ = IntroPhase::FadeIn;
    float elapsed_ = 0.0f;
    bool resourcesValid_ = false;
    bool skipRequested_ = false;
};

} // namespace skr::render
