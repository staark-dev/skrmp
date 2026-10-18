#include "render-d3d9/d3d9_hook.h"
#include "intro/urban_intro.h"
#include "lobby/lobby_manager.h"
#include <atomic>
#include <algorithm>

namespace skr::render {

namespace {

constexpr uintptr_t kGtaDevicePtr = 0x00C97C28;

EndSceneFn g_origEndScene = nullptr;
ResetFn    g_origReset    = nullptr;
void**     g_hookedVtable = nullptr;
std::atomic<bool> g_hookInstalled{false};
HANDLE g_hookThread = nullptr;
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

    // Post-intro:
    //   - lobby ENABLED  → OnEndSceneAfterIntro draws its own cover + runs
    //                      the CEF tick (Phase 1 lobby path).
    //   - lobby DISABLED → do nothing extra. The old "cover until in-game"
    //                      fade was removed because it stays opaque until
    //                      IsPlayerInGame() flips, which hides the vanilla
    //                      SA-MP menu when the launcher is NOT in
    //                      auto-connect mode (user can't click servers).
    //                      SA-MP's own "Connecting to..." / "Connected..."
    //                      strings are already blanked by samp_hooks, so
    //                      no cover is needed to hide them.
    if (!introActive) {
        if (skr::lobby::IsEnabled()) {
            skr::lobby::OnEndSceneAfterIntro(device);
        }
        g_transState = TransitionState::Done;
    }

    return g_origEndScene(device);
}

HRESULT WINAPI HookedReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* pp) {
    skr::intro::BeforeReset();
    HRESULT hr = g_origReset(device, pp);
    skr::intro::AfterReset(hr);
    return hr;
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

    bool ok = true;
    ok &= PatchVtable(g_hookedVtable, kVtableEndScene,
                       reinterpret_cast<void*>(&HookedEndScene),
                       reinterpret_cast<void**>(&g_origEndScene));
    ok &= PatchVtable(g_hookedVtable, kVtableReset,
                       reinterpret_cast<void*>(&HookedReset),
                       reinterpret_cast<void**>(&g_origReset));

    g_hookInstalled.store(ok);
    return ok ? 0 : 1;
}

} // anonymous namespace

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
    skr::intro::Shutdown();

    if (g_hookInstalled.load() && g_hookedVtable) {
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
