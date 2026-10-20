#include "render-d3d9/d3d9_hook.h"
#include "intro/urban_intro.h"
#include "intro/launch_cover.h"
#include "lobby/lobby_manager.h"
#include "render-d3d9/fps_engine.h"
#include <atomic>
#include <algorithm>
#include <cstdio>

namespace skr::render {

namespace {

constexpr uintptr_t kGtaDevicePtr = 0x00C97C28;

EndSceneFn g_origEndScene = nullptr;
ResetFn    g_origReset    = nullptr;
void**     g_hookedVtable = nullptr;
std::atomic<bool> g_hookInstalled{false};
HANDLE g_hookThread = nullptr;
using PresentFn = HRESULT(WINAPI*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
PresentFn g_origPresent = nullptr;
std::atomic<bool> g_cefPresentOwner{false};
bool g_cefDrewCover = false;
LARGE_INTEGER g_freq{};

// ── Transition overlay ──────────────────────────────────────────────
constexpr float kTransitionFadeSec = 0.8f;
enum class TransitionState { Covering, FadingOut, Done };
TransitionState g_transState = TransitionState::Covering;
float g_transFadeElapsed = 0.0f;
LARGE_INTEGER g_transLastTime{};

bool IsPlayerInGame() {
    HMODULE samp = GetModuleHandleA("samp.dll");
    if (!samp) return false;
    auto base = reinterpret_cast<uintptr_t>(samp);
    auto** ppNetGame = reinterpret_cast<void**>(base + 0x0026EB94);
    if (!*ppNetGame) return false;
    auto* pState = reinterpret_cast<uint8_t*>(
        reinterpret_cast<uintptr_t>(*ppNetGame) + 0x3CD);
    return *pState == 5;
}

void DrawFullscreenQuad(IDirect3DDevice9* device, DWORD color) {
    struct QuadVertex { float x, y, z, rhw; DWORD color; };
    constexpr DWORD kFVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;

    D3DVIEWPORT9 vp{};
    device->GetViewport(&vp);
    float w = static_cast<float>(vp.Width);
    float h = static_cast<float>(vp.Height);

    QuadVertex quad[4] = {
        {0, 0, 0, 1, color}, {w, 0, 0, 1, color},
        {0, h, 0, 1, color}, {w, h, 0, 1, color},
    };

    DWORD sv_ab, sv_sb, sv_db, sv_lt, sv_ze, sv_cl;
    device->GetRenderState(D3DRS_ALPHABLENDENABLE, &sv_ab);
    device->GetRenderState(D3DRS_SRCBLEND, &sv_sb);
    device->GetRenderState(D3DRS_DESTBLEND, &sv_db);
    device->GetRenderState(D3DRS_LIGHTING, &sv_lt);
    device->GetRenderState(D3DRS_ZENABLE, &sv_ze);
    device->GetRenderState(D3DRS_CULLMODE, &sv_cl);
    IDirect3DBaseTexture9* sv_tex = nullptr;
    device->GetTexture(0, &sv_tex);
    DWORD sv_fvf = 0;
    device->GetFVF(&sv_fvf);

    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetTexture(0, nullptr);
    device->SetFVF(kFVF);
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(QuadVertex));

    device->SetRenderState(D3DRS_ALPHABLENDENABLE, sv_ab);
    device->SetRenderState(D3DRS_SRCBLEND, sv_sb);
    device->SetRenderState(D3DRS_DESTBLEND, sv_db);
    device->SetRenderState(D3DRS_LIGHTING, sv_lt);
    device->SetRenderState(D3DRS_ZENABLE, sv_ze);
    device->SetRenderState(D3DRS_CULLMODE, sv_cl);
    device->SetTexture(0, sv_tex);
    if (sv_tex) sv_tex->Release();
    device->SetFVF(sv_fvf);
}

bool PatchVtable(void** vtable, int index, void* hook, void** original) {
    DWORD oldProtect = 0;
    void* entry = &vtable[index];
    if (!VirtualProtect(entry, sizeof(void*), PAGE_READWRITE, &oldProtect))
        return false;
    *original = vtable[index];
    vtable[index] = hook;
    VirtualProtect(entry, sizeof(void*), oldProtect, &oldProtect);
    return true;
}

HRESULT WINAPI HookedEndScene(IDirect3DDevice9* device) {
    static bool reported = false;
    if (!reported) { reported = true; fps::Trace("hook EndScene entered"); }
    if (!g_freq.QuadPart) {
        QueryPerformanceFrequency(&g_freq);
        QueryPerformanceCounter(&g_transLastTime);
    }

    bool introActive = false;
    if (!skr::intro::IsFinished()) {
        // The intro holds until the player is in game, then fades out
        if (IsPlayerInGame())
            skr::intro::SetWorldReady();
        skr::intro::OnFrame(device);
        introActive = !skr::intro::IsFinished();
        if (!introActive) QueryPerformanceCounter(&g_transLastTime);
    }

    // Lobby ENABLED (SKR\lobby.on present): Phase 1 lobby path draws its own
    // cover and runs the CEF tick, replacing the old transition overlay.
    //
    // Lobby DISABLED (default): behavior is identical to pre-Phase-1 — the
    // Covering → FadingOut → Done transition hides SA-MP's connect frames
    // and then yields to vanilla GTA. Restored verbatim from 35e30e1 so
    // "lobby off" means the exact pre-lobby experience.
    if (!introActive) {
        if (skr::lobby::IsEnabled()) {
            skr::lobby::OnEndSceneAfterIntro(device);
            g_transState = TransitionState::Done;
        } else if (g_transState != TransitionState::Done) {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            float dt = static_cast<float>(now.QuadPart - g_transLastTime.QuadPart)
                       / static_cast<float>(g_freq.QuadPart);
            g_transLastTime = now;
            if (dt > 0.1f) dt = 0.1f;

            if (g_transState == TransitionState::Covering) {
                DrawFullscreenQuad(device, 0xFF000000);
                if (IsPlayerInGame())
                    g_transState = TransitionState::FadingOut;
            } else if (g_transState == TransitionState::FadingOut) {
                g_transFadeElapsed += dt;
                float alpha = 1.0f - (g_transFadeElapsed / kTransitionFadeSec);
                alpha = (std::max)(0.0f, (std::min)(1.0f, alpha));
                if (alpha > 0.001f) {
                    DWORD a = static_cast<DWORD>(alpha * 255.0f);
                    DrawFullscreenQuad(device, (a << 24) | 0x000000);
                }
                if (g_transFadeElapsed >= kTransitionFadeSec)
                    g_transState = TransitionState::Done;
            }
        }
    }

    return g_origEndScene(device);
}

HRESULT WINAPI HookedReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* pp) {
    fps::Trace("hook Reset entered");
    skr::render::fps::BeforeReset();
    skr::launch_cover::BeforeReset();
    skr::intro::BeforeReset();
    HRESULT hr = g_origReset(device, pp);
    char message[80]{}; sprintf_s(message, "hook Reset returned 0x%08lX", hr); fps::Trace(message);
    skr::intro::AfterReset(hr);
    return hr;
}

HRESULT WINAPI HookedPresent(
    IDirect3DDevice9* device,
    const RECT* source,
    const RECT* dest,
    HWND window,
    const RGNDATA* dirty)
{
    if (g_cefPresentOwner.load()) return g_origPresent(device, source, dest, window, dirty);
    const bool inWorld = IsPlayerInGame();
    static ULONGLONG lastDiagnostic = 0;

    const bool drawn = skr::launch_cover::Draw(
        device, inWorld
    );
    if (GetTickCount64() - lastDiagnostic >= 5000) {
        lastDiagnostic = GetTickCount64();
        char message[120]{};
        sprintf_s(message, "hook Present inWorld=%u cover=%u introFinished=%u",
                  inWorld ? 1u : 0u, drawn ? 1u : 0u, skr::intro::IsFinished() ? 1u : 0u);
        fps::Trace(message);
    }

#ifdef SKR_EXTERNAL_LOBBY
    // Diagnostic: testăm FPS fără condiția SA-MP.
    if (!drawn && skr::intro::IsFinished()) {
        skr::render::fps::Draw(device);
    }
#endif

    HRESULT result = g_origPresent(
        device, source, dest, window, dirty
    );

    skr::launch_cover::Presented(drawn, result);

#ifdef SKR_EXTERNAL_LOBBY
    // Diagnostic: permitem acumularea FPS
    // fără a depinde de IsPlayerInGame().
    skr::render::fps::Presented(
        result,
        !drawn && skr::intro::IsFinished()
    );
#endif

    return result;
}

DWORD WINAPI D3D9HookThread(LPVOID) {
    auto** ppDevice = reinterpret_cast<IDirect3DDevice9**>(kGtaDevicePtr);
    for (int i = 0; i < 600; ++i) {
        if (*ppDevice != nullptr) break;
        Sleep(50);
    }

    IDirect3DDevice9* device = *ppDevice;
    if (!device) return 1;

    g_hookedVtable = *reinterpret_cast<void***>(device);
#ifdef SKR_EXTERNAL_LOBBY
    skr::render::fps::Initialize();
#endif

    bool ok = true;
#ifdef SKR_EXTERNAL_LOBBY
    ok &= PatchVtable(g_hookedVtable, 17, reinterpret_cast<void*>(&HookedPresent), reinterpret_cast<void**>(&g_origPresent));
#endif
    ok &= PatchVtable(g_hookedVtable, kVtableEndScene,
                       reinterpret_cast<void*>(&HookedEndScene),
                       reinterpret_cast<void**>(&g_origEndScene));
    ok &= PatchVtable(g_hookedVtable, kVtableReset,
                       reinterpret_cast<void*>(&HookedReset),
                       reinterpret_cast<void**>(&g_origReset));

    g_hookInstalled.store(ok);
    fps::Trace(ok ? "D3D9 vtable hooks installed" : "D3D9 vtable hook install failed");
    return ok ? 0 : 1;
}

} // anonymous namespace

// SKR-owned optional ABI, invoked by our pinned omp-cef fork after its rendering.
// One owner prevents duplicate drawing/counting through nested D3D9 wrappers.
extern "C" __declspec(dllexport) void __cdecl SkrBeforePresentV1(IDirect3DDevice9* device) {
    if (!g_cefPresentOwner.exchange(true)) fps::Trace("CEF real-device Present owner acquired");
    g_cefDrewCover = skr::launch_cover::Draw(device, IsPlayerInGame());
    if (!g_cefDrewCover && skr::intro::IsFinished()) fps::Draw(device);
}
extern "C" __declspec(dllexport) void __cdecl SkrAfterPresentV1(HRESULT result) {
    skr::launch_cover::Presented(g_cefDrewCover, result);
    fps::Presented(result, !g_cefDrewCover && skr::intro::IsFinished());
}
extern "C" __declspec(dllexport) void __cdecl SkrBeforeResetV1() {
    fps::Trace("CEF real-device Reset entered");
    fps::BeforeReset(); skr::launch_cover::BeforeReset(); skr::intro::BeforeReset();
}
extern "C" __declspec(dllexport) BOOL __cdecl SkrEnhancedConfirmedV1() {
    return skr::lobby::IsSkrSession() ? TRUE : FALSE;
}

bool InstallD3D9Hooks() {
    g_hookThread = CreateThread(nullptr, 0, D3D9HookThread, nullptr, 0, nullptr);
    return g_hookThread != nullptr;
}

void RemoveD3D9Hooks() {
    if (g_hookThread) {
        WaitForSingleObject(g_hookThread, 5000);
        CloseHandle(g_hookThread);
        g_hookThread = nullptr;
    }
    skr::render::fps::Shutdown();
    skr::intro::Shutdown();

    if (g_hookInstalled.load() && g_hookedVtable) {
        if (g_origPresent && g_hookedVtable[17] == reinterpret_cast<void*>(&HookedPresent)) {
            DWORD old = 0;
            if (VirtualProtect(&g_hookedVtable[17], sizeof(void*), PAGE_READWRITE, &old)) {
                g_hookedVtable[17] = reinterpret_cast<void*>(g_origPresent);
                VirtualProtect(&g_hookedVtable[17], sizeof(void*), old, &old);
            }
        }
        if (g_origEndScene) {
            DWORD old = 0;
            void* entry = &g_hookedVtable[kVtableEndScene];
            VirtualProtect(entry, sizeof(void*), PAGE_READWRITE, &old);
            g_hookedVtable[kVtableEndScene] = reinterpret_cast<void*>(g_origEndScene);
            VirtualProtect(entry, sizeof(void*), old, &old);
        }
        if (g_origReset) {
            DWORD old = 0;
            void* entry = &g_hookedVtable[kVtableReset];
            VirtualProtect(entry, sizeof(void*), PAGE_READWRITE, &old);
            g_hookedVtable[kVtableReset] = reinterpret_cast<void*>(g_origReset);
            VirtualProtect(entry, sizeof(void*), old, &old);
        }
    }

    g_origEndScene = nullptr;
    g_origReset = nullptr;
    g_hookInstalled.store(false);
}

} // namespace skr::render
