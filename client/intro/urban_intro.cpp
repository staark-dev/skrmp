#include "intro/urban_intro.h"
#include "intro/intro_audio.h"
#include "intro/intro_timeline.h"
#include "intro/intro_transport.h"

#include <windows.h>
#include <wincodec.h>
#include <bcrypt.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace skr::intro {

namespace {

// ── Logging (SKR\intro.log next to SKR.Client.dll) ──────────────────
std::mutex g_logMutex;
std::wstring g_baseDir;   // "<dir of SKR.Client.dll>\SKR\"

void Log(const char* fmt, ...) {
    if (g_baseDir.empty()) return;
    std::lock_guard<std::mutex> lock(g_logMutex);
    FILE* f = _wfopen((g_baseDir + L"intro.log").c_str(), L"a");
    if (!f) return;
    fprintf(f, "[%lu] ", GetTickCount());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

std::wstring ModuleDir(HMODULE mod) {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(mod, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return {};
    std::wstring s(path, n);
    size_t slash = s.find_last_of(L'\\');
    return slash == std::wstring::npos ? std::wstring() : s.substr(0, slash + 1);
}

std::string Narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

// SHA-256 of a file for the test profile in the log.
std::string Sha256File(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "missing";
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string out = "error";
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<unsigned char> buf(1 << 16);
        DWORD read = 0;
        while (ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &read, nullptr) && read)
            BCryptHashData(hash, buf.data(), read, 0);
        unsigned char digest[32];
        if (BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
            char hex[65];
            for (int i = 0; i < 32; ++i) snprintf(hex + i * 2, 3, "%02x", digest[i]);
            out = hex;
        }
    }
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(h);
    return out;
}

// ── Images ──────────────────────────────────────────────────────────
struct Image {
    std::wstring file;
    UINT w = 0, h = 0;
    std::vector<unsigned char> bgra;   // straight (non-premultiplied) BGRA
    IDirect3DTexture9* tex = nullptr;
};

bool DecodePng(IWICImagingFactory* wic, const std::wstring& path, Image& img, std::string& err) {
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    bool ok = false;
    HRESULT hr = wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                WICDecodeMetadataCacheOnDemand, &dec);
    if (SUCCEEDED(hr)) hr = dec->GetFrame(0, &frame);
    if (SUCCEEDED(hr)) hr = wic->CreateFormatConverter(&conv);
    if (SUCCEEDED(hr)) hr = conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA,
                                             WICBitmapDitherTypeNone, nullptr, 0.0,
                                             WICBitmapPaletteTypeCustom);
    if (SUCCEEDED(hr)) hr = conv->GetSize(&img.w, &img.h);
    if (SUCCEEDED(hr) && img.w && img.h && img.w <= 8192 && img.h <= 8192) {
        img.bgra.resize(static_cast<size_t>(img.w) * img.h * 4);
        hr = conv->CopyPixels(nullptr, img.w * 4, static_cast<UINT>(img.bgra.size()), img.bgra.data());
        ok = SUCCEEDED(hr);
    }
    if (!ok) {
        char buf[64];
        snprintf(buf, sizeof(buf), "WIC decode failed (hr=0x%08lX)", static_cast<unsigned long>(hr));
        err = buf;
    }
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    return ok;
}

bool Upload(IDirect3DDevice9* dev, const D3DCAPS9& caps, Image& img) {
    if (img.w > caps.MaxTextureWidth || img.h > caps.MaxTextureHeight) {
        Log("texture %s %ux%u exceeds device max %lux%lu", Narrow(img.file).c_str(), img.w, img.h,
            caps.MaxTextureWidth, caps.MaxTextureHeight);
        return false;
    }
    bool pow2 = (img.w & (img.w - 1)) == 0 && (img.h & (img.h - 1)) == 0;
    if (!pow2 && (caps.TextureCaps & D3DPTEXTURECAPS_POW2) &&
        !(caps.TextureCaps & D3DPTEXTURECAPS_NONPOW2CONDITIONAL)) {
        Log("texture %s is NPOT and the device supports only power-of-two textures",
            Narrow(img.file).c_str());
        return false;
    }
    // MANAGED pool: survives Reset; no mip levels, clamp addressing (NONPOW2CONDITIONAL rules).
    HRESULT hr = dev->CreateTexture(img.w, img.h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &img.tex, nullptr);
    if (FAILED(hr)) { Log("CreateTexture %s failed hr=0x%08lX", Narrow(img.file).c_str(), hr); return false; }
    D3DLOCKED_RECT lr{};
    hr = img.tex->LockRect(0, &lr, nullptr, 0);
    if (FAILED(hr)) {
        Log("LockRect %s failed hr=0x%08lX", Narrow(img.file).c_str(), hr);
        img.tex->Release();
        img.tex = nullptr;
        return false;
    }
    for (UINT y = 0; y < img.h; ++y)
        memcpy(static_cast<unsigned char*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch,
               img.bgra.data() + static_cast<size_t>(y) * img.w * 4, img.w * 4);
    img.tex->UnlockRect(0);
    img.bgra.clear();
    img.bgra.shrink_to_fit();
    return true;
}

// ── State ───────────────────────────────────────────────────────────
enum class Phase { NotStarted, Loading, Ready, Playing, Done };

std::atomic<Phase> g_phase{Phase::NotStarted};
std::atomic<bool> g_loadOk{false};
std::atomic<bool> g_workerExit{false};
std::atomic<ULONGLONG> g_lastFrameTick{0};
HANDLE g_worker = nullptr;
HANDLE g_wake = nullptr;                    // signals the worker to tear down audio

Image g_bg, g_fg, g_logo;
Image g_text[4];
IntroAudio g_audio;
bool g_audioOk = false;
Transport g_transport;
bool g_deviceLost = false;
std::atomic<bool> g_worldReady{false};
bool g_paused = false;
HWND g_focusWindow = nullptr;
LARGE_INTEGER g_qpcFreq{};

double Now() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / static_cast<double>(g_qpcFreq.QuadPart);
}

std::vector<Image*> AllImages() {
    std::vector<Image*> v = {&g_bg, &g_fg, &g_logo};
    for (auto& t : g_text) v.push_back(&t);
    return v;
}

void ReleaseTextures() {
    for (Image* img : AllImages()) {
        if (img->tex) { img->tex->Release(); img->tex = nullptr; }
        img->bgra.clear();
        img->bgra.shrink_to_fit();
    }
}

DWORD WINAPI Worker(LPVOID) {
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    HMODULE gta = GetModuleHandleW(nullptr);
    HMODULE samp = GetModuleHandleW(L"samp.dll");
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&Worker), &self);
    wchar_t p[MAX_PATH];
    if (GetModuleFileNameW(gta, p, MAX_PATH)) Log("profile gta: %s sha256=%s", Narrow(p).c_str(), Sha256File(p).c_str());
    if (samp && GetModuleFileNameW(samp, p, MAX_PATH)) Log("profile samp: %s sha256=%s", Narrow(p).c_str(), Sha256File(p).c_str());
    if (self && GetModuleFileNameW(self, p, MAX_PATH)) Log("profile client: %s sha256=%s", Narrow(p).c_str(), Sha256File(p).c_str());

    bool ok = SUCCEEDED(co) || co == RPC_E_CHANGED_MODE;
    IWICImagingFactory* wic = nullptr;
    if (ok && FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&wic)))) {
        Log("WIC factory unavailable; intro disabled");
        ok = false;
    }
    if (ok) {
        for (Image* img : AllImages()) {
            std::wstring path = g_baseDir + L"urban\\" + img->file;
            std::string err;
            if (!DecodePng(wic, path, *img, err)) {
                Log("asset %s: %s; intro disabled", Narrow(path).c_str(), err.c_str());
                ok = false;
                break;
            }
            Log("asset %s %ux%u", Narrow(img->file).c_str(), img->w, img->h);
        }
    }
    if (wic) wic->Release();

    if (ok) {
        std::string err;
        g_audioOk = g_audio.Open(g_baseDir + L"audio\\intro.mp3", err);
        Log(g_audioOk ? "audio ready (miniaudio 0.11.23, f32 2ch 48 kHz)"
                      : "audio unavailable (%s); visual intro continues on the monotonic clock",
            err.c_str());
    }

    g_loadOk.store(ok);
    g_phase.store(ok ? Phase::Ready : Phase::Done);

    // Watchdog + teardown owner for audio: no frames for 250 ms pauses the music; the
    // next drawn frame resumes it.
    bool pausedByWatchdog = false;
    while (!g_workerExit.load()) {
        WaitForSingleObject(g_wake, 100);
        if (g_phase.load() == Phase::Done) break;
        if (g_phase.load() != Phase::Playing) continue;
        ULONGLONG gap = GetTickCount64() - g_lastFrameTick.load();
        if (gap > 250 && !pausedByWatchdog) {
            g_audio.Pause();
            pausedByWatchdog = true;
            Log("watchdog: no frames for %llu ms, audio paused", gap);
        }
        if (gap <= 250) pausedByWatchdog = false;
    }
    g_audio.Close();
    Log("audio closed");
    if (SUCCEEDED(co)) CoUninitialize();
    return 0;
}

void BeginLoading() {
    QueryPerformanceFrequency(&g_qpcFreq);
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&BeginLoading), &self);
    g_baseDir = ModuleDir(self) + L"SKR\\";
    g_bg.file = L"background.png";
    g_fg.file = L"foreground.png";
    g_logo.file = L"logo.png";
    for (int i = 0; i < 4; ++i) g_text[i].file = kTextLayers[i].file;

    Log("intro start; assets in %s", Narrow(g_baseDir).c_str());
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_phase.store(Phase::Loading);
    g_worker = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    if (!g_worker) { Log("worker thread creation failed; intro disabled"); g_phase.store(Phase::Done); }
}

void Finish(const char* why) {
    if (g_phase.load() == Phase::Done) return;
    Log("intro finished: %s (t=%.3f, audio clock=%d)", why, g_transport.Time(),
        g_transport.UsingAudioClock() ? 1 : 0);
    g_transport.Finish();
    g_phase.store(Phase::Done);
    if (g_wake) SetEvent(g_wake);   // worker closes audio
    ReleaseTextures();
}

// ── Drawing ─────────────────────────────────────────────────────────
struct Vtx { float x, y, z, rhw; DWORD c; float u, v; };

DWORD Diffuse(float rgb, float a) {
    auto b = [](float x) { return static_cast<DWORD>(Clamp01(x) * 255.0f + 0.5f); };
    return D3DCOLOR_ARGB(b(a), b(rgb), b(rgb), b(rgb));
}

void Quad(IDirect3DDevice9* dev, const Placement& pl, const Rect& r, const Uv& uv, DWORD c) {
    float x0 = pl.ox + r.x * pl.s - 0.5f, y0 = pl.oy + r.y * pl.s - 0.5f;
    float x1 = x0 + r.w * pl.s, y1 = y0 + r.h * pl.s;
    Vtx v[4] = {{x0, y0, 0, 1, c, uv.u0, uv.v0}, {x1, y0, 0, 1, c, uv.u1, uv.v0},
                {x0, y1, 0, 1, c, uv.u0, uv.v1}, {x1, y1, 0, 1, c, uv.u1, uv.v1}};
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vtx));
}

void SetTextured(IDirect3DDevice9* dev, IDirect3DTexture9* tex) {
    dev->SetTexture(0, tex);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
}

void SetUntextured(IDirect3DDevice9* dev) {
    dev->SetTexture(0, nullptr);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
}

// progress: 0..1 (LoadProgress); waiting: GTA loaded, server join pending (pulse shown).
void Draw(IDirect3DDevice9* dev, float t, float progress, bool waiting, double now) {
    IDirect3DSurface9* rt = nullptr;
    if (FAILED(dev->GetRenderTarget(0, &rt)) || !rt) return;
    D3DSURFACE_DESC desc{};
    rt->GetDesc(&desc);
    rt->Release();

    // Created per frame so nothing of ours is alive when the host calls Reset, even if a
    // Reset happens outside the hook we own. CreateStateBlock records the current state.
    IDirect3DStateBlock9* sb = nullptr;
    if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &sb)) || !sb)
        return;   // without a way to restore host state we draw nothing

    D3DVIEWPORT9 vp = {0, 0, desc.Width, desc.Height, 0.0f, 1.0f};
    dev->SetViewport(&vp);
    dev->SetVertexShader(nullptr);
    dev->SetPixelShader(nullptr);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);       // straight BGRA from WIC
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);

    // A. Opaque black matte over the whole back buffer (no Clear).
    Placement full{1.0f, 0.0f, 0.0f};
    SetUntextured(dev);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    Quad(dev, full, {0, 0, static_cast<float>(desc.Width), static_cast<float>(desc.Height)},
         {0, 0, 1, 1}, D3DCOLOR_ARGB(255, 0, 0, 0));

    // Everything else is clipped to the 16:9 composition rectangle.
    Placement pl = Place(static_cast<float>(desc.Width), static_cast<float>(desc.Height));
    RECT sc = {static_cast<LONG>(pl.ox), static_cast<LONG>(pl.oy),
               static_cast<LONG>(pl.ox + kCanvasW * pl.s + 0.5f), static_cast<LONG>(pl.oy + kCanvasH * pl.s + 0.5f)};
    dev->SetScissorRect(&sc);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);

    float k = MasterFade(t);
    auto drawLayer = [&](Image& img, const Layer& l) {
        if (!img.tex || l.alpha <= 0.0f) return;
        SetTextured(dev, img.tex);
        Quad(dev, pl, l.rect, l.uv, Diffuse(k, l.alpha));
    };

    drawLayer(g_bg, Background(t));                 // B
    drawLayer(g_fg, Foreground(t));                 // C
    drawLayer(g_logo, Logo(t));                     // D

    Layer footer = Footer(t);                       // E
    if (footer.alpha > 0.0f) {
        SetUntextured(dev);
        auto ch = [&](unsigned shift) {
            return static_cast<DWORD>(((kFooterRgb >> shift) & 0xFF) * k + 0.5f);
        };
        DWORD a = static_cast<DWORD>(Clamp01(footer.alpha) * 255.0f + 0.5f);
        Quad(dev, pl, footer.rect, footer.uv, D3DCOLOR_ARGB(a, ch(16), ch(8), ch(0)));
    }

    for (int i = 0; i < 4; ++i) drawLayer(g_text[i], Text(kTextLayers[i], t));   // F

    // Loading bar: fades in with the footer, follows the master fade on RGB.
    if (footer.alpha > 0.0f) {
        SetUntextured(dev);
        auto rgb = [&](unsigned color, float a) {
            auto ch = [&](unsigned shift) { return static_cast<DWORD>(((color >> shift) & 0xFF) * k + 0.5f); };
            DWORD al = static_cast<DWORD>(Clamp01(a) * 255.0f + 0.5f);
            return D3DCOLOR_ARGB(al, ch(16), ch(8), ch(0));
        };
        Quad(dev, pl, kBarRect, {0, 0, 1, 1}, rgb(0xFFFFFF, 0.15f * footer.alpha));
        Rect fill = kBarRect;
        fill.w = kBarRect.w * Clamp01(progress);
        if (fill.w > 0.0f) Quad(dev, pl, fill, {0, 0, 1, 1}, rgb(kBarRgb, footer.alpha));
        if (waiting) {
            // Server join pending: the unfilled part breathes smoothly (no jumping segment).
            Rect rest = {fill.x + fill.w, kBarRect.y, kBarRect.w - fill.w, kBarRect.h};
            float breath = 0.5f - 0.5f * std::cos(static_cast<float>(now) * 3.14159265f);   // 2 s period
            if (rest.w > 0.0f) Quad(dev, pl, rest, {0, 0, 1, 1}, rgb(kBarRgb, 0.30f * breath * footer.alpha));
        }
    }

    sb->Apply();
    sb->Release();
}

void DrawBlack(IDirect3DDevice9* device) {
    IDirect3DStateBlock9* sb = nullptr;
    if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &sb)) || !sb) return;
    IDirect3DSurface9* rt = nullptr;
    if (SUCCEEDED(device->GetRenderTarget(0, &rt)) && rt) {
        D3DSURFACE_DESC d{};
        rt->GetDesc(&d);
        rt->Release();
        D3DVIEWPORT9 vp = {0, 0, d.Width, d.Height, 0.0f, 1.0f};
        device->SetViewport(&vp);
        device->SetVertexShader(nullptr);
        device->SetPixelShader(nullptr);
        device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
        device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        SetUntextured(device);
        Placement full{1.0f, 0.0f, 0.0f};
        Quad(device, full, {0, 0, static_cast<float>(d.Width), static_cast<float>(d.Height)},
             {0, 0, 1, 1}, D3DCOLOR_ARGB(255, 0, 0, 0));
    }
    sb->Apply();
    sb->Release();
}

// ── Frame statistics (one log line per second while active) ─────────
ULONGLONG g_statStart = 0;
int g_statFrames = 0;
ULONGLONG g_statMaxGap = 0;

void CountFrame(ULONGLONG tick, ULONGLONG gap, const char* source) {
    if (!g_statStart) g_statStart = tick;
    ++g_statFrames;
    if (gap > g_statMaxGap && gap < 60000) g_statMaxGap = gap;
    if (tick - g_statStart >= 1000) {
        Log("frames=%d in %llu ms (max gap %llu ms) via %s, t=%.2f clock=%.2f hold=%d",
            g_statFrames, tick - g_statStart, g_statMaxGap, source, g_transport.Time(),
            g_transport.Clock(), g_transport.WorldReady() ? 0 : 1);
        g_statStart = tick;
        g_statFrames = 0;
        g_statMaxGap = 0;
    }
}

// One intro frame on `device` (the real device on the SA-MP Present path).
void Frame(IDirect3DDevice9* device, const char* source) {
    Phase phase = g_phase.load();
    if (phase == Phase::Done) return;
    if (phase == Phase::NotStarted) { BeginLoading(); phase = g_phase.load(); }
    if (g_deviceLost || device->TestCooperativeLevel() != D3D_OK) return;

    if (phase == Phase::Ready) {
        D3DCAPS9 caps{};
        device->GetDeviceCaps(&caps);
        for (Image* img : AllImages()) {
            if (!Upload(device, caps, *img)) { Finish("texture upload failed"); return; }
        }
        D3DDEVICE_CREATION_PARAMETERS cp{};
        if (SUCCEEDED(device->GetCreationParameters(&cp))) g_focusWindow = cp.hFocusWindow;
        double now = Now();
        g_transport.Start(now);
        if (g_audioOk) g_audio.Start();
        g_lastFrameTick.store(GetTickCount64());
        g_phase.store(Phase::Playing);
        Log("playback started (backbuffer textures uploaded)");
        phase = Phase::Playing;
    }

    if (phase == Phase::Loading) {
        DrawBlack(device);   // keep the screen black while assets decode on the worker
        return;
    }

    double now = Now();
    ULONGLONG tick = GetTickCount64();
    ULONGLONG gap = tick - g_lastFrameTick.exchange(tick);
    CountFrame(tick, gap, source);
    bool focused = !g_focusWindow || GetForegroundWindow() == g_focusWindow;

    if (!focused) {
        if (!g_paused) {
            g_transport.Pause();
            if (g_audioOk) g_audio.Pause();
            g_paused = true;
            Log("paused: focus lost (t=%.3f)", g_transport.Time());
        }
    } else if (g_paused || gap > 250) {
        // Resume after focus return or after the game stopped sending frames.
        g_transport.Pause();
        g_transport.Resume(now);
        if (g_audioOk) g_audio.Resume();
        if (g_paused) Log("resumed (t=%.3f)", g_transport.Time());
        g_paused = false;
    }

    // Same SA-MP R5 check as d3d9_hook's IsPlayerInGame: CNetGame* at samp+0x26EB94,
    // game state byte +0x3CD == 5 (connected, after the server's join reply).
    bool connected = false;
    if (HMODULE samp = GetModuleHandleW(L"samp.dll")) {
        auto* netGame = *reinterpret_cast<uint8_t**>(reinterpret_cast<uintptr_t>(samp) + 0x26EB94);
        connected = netGame && netGame[0x3CD] == 5;
    }
    if (connected) g_worldReady.store(true);
    if (!g_worldReady.load() && g_transport.HoldOffset() > kMaxHold) {
        Log("no server join after %.0f s of hold; fading out so SA-MP connection messages show", kMaxHold);
        g_worldReady.store(true);
    }
    bool ready = g_worldReady.load();
    if (ready && !g_transport.WorldReady())
        Log("world ready at t=%.3f (held %.3f s); final fade", g_transport.Time(), g_transport.HoldOffset());
    g_transport.SetWorldReady(ready);
    double t = g_transport.Update(now, g_audioOk ? g_audio.Position() : -1.0);
    if (g_audioOk) g_audio.SetHold(g_transport.WorldReady(), g_transport.HoldOffset());

    if (focused && g_transport.OnSkipKey((GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0)) {
        Finish("skipped with Escape");
        return;
    }
    if (g_transport.GetState() == Transport::State::Finished) { Finish("completed"); return; }

    // plugin-sdk CLoadingScreen::m_numChunksLoaded (GTA 1.0 US 0x8D0940).
    int chunks = *reinterpret_cast<int*>(0x8D0940);
    float progress = LoadProgress(chunks, connected);
    bool waiting = !connected && chunks >= kGtaLoadingChunks;
    Draw(device, static_cast<float>(t), progress, waiting, now);
}

// ── SA-MP Present-proxy draw point ─────────────────────────────────
// samp.dll 0.3.7-R5 (sha256 b72b5dbe...), disassembled locally: D3D9Proxy_Present
// (RVA 0x75730) converges at RVA 0x75C65 from every branch, then unconditionally runs
// `call 0x1009D8B0` (RVA 0x75C73; cdecl, no args, plain ret) after all SA-MP UI and
// right before the real Present ([RVA 0x26EB40] = real IDirect3DDevice9*, vtable +0x44).
// The SKRClient EndScene hook stops being called once other modules take over the
// device, so the intro draws here; EndScene is only a fallback before samp.dll loads.
constexpr uintptr_t kPrePresentCallRva = 0x75C73;
constexpr uintptr_t kPrePresentFnRva = 0x9D8B0;
constexpr uintptr_t kRealDeviceRva = 0x26EB40;

using VoidFn = void(__cdecl*)();
VoidFn g_origPrePresent = nullptr;
uintptr_t g_sampBase = 0;
std::atomic<bool> g_presentHooked{false};
bool g_inDraw = false;
ULONGLONG g_lastPresentDraw = 0;

IDirect3DDevice9* RealDevice() {
    if (!g_sampBase) return nullptr;
    return *reinterpret_cast<IDirect3DDevice9**>(g_sampBase + kRealDeviceRva);
}

void __cdecl HookedPrePresent() {
    if (!g_inDraw && g_phase.load() != Phase::Done) {
        if (IDirect3DDevice9* dev = RealDevice()) {
            g_lastPresentDraw = GetTickCount64();
            g_inDraw = true;
            // SA-MP draws its UI here without a scene of its own; open one only if none is open.
            bool began = SUCCEEDED(dev->BeginScene());
            Frame(dev, "samp-present");
            if (began) dev->EndScene();
            g_inDraw = false;
        }
    }
    g_origPrePresent();
}

void InstallPresentHook() {
    if (g_presentHooked.load()) return;
    HMODULE samp = GetModuleHandleW(L"samp.dll");
    if (!samp) return;
    uintptr_t base = reinterpret_cast<uintptr_t>(samp);
    auto* site = reinterpret_cast<uint8_t*>(base + kPrePresentCallRva);
    if (site[0] != 0xE8 ||
        base + kPrePresentCallRva + 5 + *reinterpret_cast<int32_t*>(site + 1) != base + kPrePresentFnRva) {
        static bool logged = false;
        if (!logged) { Log("samp present call site does not match R5; using EndScene only"); logged = true; }
        return;
    }
    g_sampBase = base;
    g_origPrePresent = reinterpret_cast<VoidFn>(base + kPrePresentFnRva);
    int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&HookedPrePresent) -
                                       (base + kPrePresentCallRva + 5));
    DWORD old = 0;
    if (!VirtualProtect(site + 1, 4, PAGE_EXECUTE_READWRITE, &old)) return;
    *reinterpret_cast<int32_t*>(site + 1) = rel;
    VirtualProtect(site + 1, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    g_presentHooked.store(true);
    Log("drawing from samp present proxy (samp+0x75C73)");
}

} // namespace

void OnFrame(IDirect3DDevice9* device) {
    if (g_inDraw || g_phase.load() == Phase::Done) return;
    if (g_phase.load() == Phase::NotStarted) BeginLoading();
    InstallPresentHook();
    // SA-MP's Present path skips its UI block while GTA is still loading; EndScene covers
    // those frames until the Present path draws, so SA-MP's own splash never shows.
    if (g_presentHooked.load() && GetTickCount64() - g_lastPresentDraw < 200) return;
    // Same device on both paths, so textures never cross between the proxy and the real one.
    IDirect3DDevice9* real = RealDevice();
    g_inDraw = true;
    Frame(real ? real : device, "endscene");
    g_inDraw = false;
}

void SetWorldReady() { g_worldReady.store(true); }

void Skip() {
    if (g_phase.load() == Phase::Playing || g_phase.load() == Phase::Ready) Finish("skipped by host");
}

void BeforeReset() {
    g_deviceLost = true;
    if (g_phase.load() == Phase::Playing && !g_paused) {
        g_transport.Pause();
        if (g_audioOk) g_audio.Pause();
        g_paused = true;
        Log("paused: device Reset (t=%.3f)", g_transport.Time());
    }
}

void AfterReset(HRESULT hr) {
    if (SUCCEEDED(hr)) {
        g_deviceLost = false;   // MANAGED textures survive; OnFrame resumes the transport
        Log("Reset OK");
    } else {
        Log("Reset failed hr=0x%08lX; no drawing until a successful Reset", hr);
    }
}

bool IsFinished() { return g_phase.load() == Phase::Done; }

void Shutdown() {
    if (g_phase.load() != Phase::NotStarted) {
        g_phase.store(Phase::Done);
        g_workerExit.store(true);
        if (g_wake) SetEvent(g_wake);
    }
    if (g_worker) {
        WaitForSingleObject(g_worker, 5000);
        CloseHandle(g_worker);
        g_worker = nullptr;
    }
    if (g_wake) { CloseHandle(g_wake); g_wake = nullptr; }
    g_audio.Close();
    ReleaseTextures();
}

} // namespace skr::intro
