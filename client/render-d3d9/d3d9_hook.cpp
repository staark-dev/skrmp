#include "render-d3d9/d3d9_hook.h"
#include "intro/urban_intro.h"
#include "intro/launch_cover.h"
#include "lobby/lobby_manager.h"
#include "render-d3d9/fps_engine.h"
#include "render-d3d9/present_chain.h"
#include "native-pause/native_pause.h"
#include <atomic>
#include <algorithm>
#include <cstdio>

namespace skr::render {

namespace {

constexpr uintptr_t kGtaDevicePtr = 0x00C97C28;

std::atomic<EndSceneFn> g_origEndScene{nullptr};
std::atomic<ResetFn>    g_origReset{nullptr};
void**     g_hookedVtable = nullptr;
std::atomic<bool> g_hookInstalled{false};
std::atomic<bool> g_hookShutdown{false};
std::atomic<skr::bootstrap::ComponentState> g_hookState{
    skr::bootstrap::ComponentState::NOT_STARTED};
HANDLE g_hookThread = nullptr;
using present_chain::PresentFn;
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
    if (!vtable || !hook || !original) return false;
    void* candidate = vtable[index];
    if (!candidate || candidate == hook) return false;
    MEMORY_BASIC_INFORMATION targetInfo{};
    if (!VirtualQuery(candidate, &targetInfo, sizeof(targetInfo)) ||
        targetInfo.State != MEM_COMMIT ||
        (targetInfo.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                               PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) == 0)
        return false;
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
    static thread_local bool inEndScene = false;
    if (inEndScene) {
        static std::atomic<bool> reportedRecursion{false};
        if (!reportedRecursion.exchange(true))
            fps::Trace("D3D9 EndScene chain recursion blocked");
        return D3D_OK;
    }
    struct Scope { bool& value; explicit Scope(bool& v) : value(v) { value = true; } ~Scope() { value = false; } } scope(inEndScene);
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

    // Native pause menu overlay (renders on top of everything)
    skr::native_pause::OnEndScene(device);

    EndSceneFn next = g_origEndScene.load(std::memory_order_acquire);
    if (!next || next == &HookedEndScene) {
        fps::Trace("D3D9 EndScene chain has no valid downstream target");
        return D3DERR_INVALIDCALL;
    }
    return next(device);
}

HRESULT WINAPI HookedReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* pp) {
    static thread_local bool inReset = false;
    if (inReset) {
        static std::atomic<bool> reportedRecursion{false};
        if (!reportedRecursion.exchange(true))
            fps::Trace("D3D9 Reset chain recursion blocked");
        return D3DERR_INVALIDCALL;
    }
    struct Scope { bool& value; explicit Scope(bool& v) : value(v) { value = true; } ~Scope() { value = false; } } scope(inReset);
    ResetFn next = g_origReset.load(std::memory_order_acquire);
    if (!next || next == &HookedReset) {
        fps::Trace("D3D9 Reset chain has no valid downstream target");
        return D3DERR_INVALIDCALL;
    }
    fps::Trace("hook Reset entered");
    skr::native_pause::OnBeforeReset();
    skr::render::fps::BeforeReset();
    skr::launch_cover::BeforeReset();
    skr::intro::BeforeReset();
    HRESULT hr = next(device, pp);
    char message[80]{}; sprintf_s(message, "hook Reset returned 0x%08lX", hr); fps::Trace(message);
    skr::intro::AfterReset(hr);
    if (SUCCEEDED(hr)) skr::native_pause::OnAfterReset(device);
    return hr;
}

// Per-frame SKR work, run once per frame at the outermost Present hook
// (present_chain.h). `next` is the hooked entry's original.
HRESULT PresentFrame(
    PresentFn next,
    IDirect3DDevice9* device,
    const RECT* source,
    const RECT* dest,
    HWND window,
    const RGNDATA* dirty)
{
    static ULONGLONG lastDiagnostic = 0;
    if (GetTickCount64() - lastDiagnostic >= 5000) {
        lastDiagnostic = GetTickCount64();
        char message[160]{};
        sprintf_s(message, "hook Present frames primary=%u rechained=%u owner=%s inWorld=%u",
                  present_chain::FrameCount(present_chain::Primary),
                  present_chain::FrameCount(present_chain::Rechained),
                  g_cefPresentOwner.load() ? "cef" : "skr", IsPlayerInGame() ? 1u : 0u);
        fps::Trace(message);
    }
    if (g_cefPresentOwner.load()) return next(device, source, dest, window, dirty);
    const bool inWorld = IsPlayerInGame();

    const bool drawn = skr::launch_cover::Draw(
        device, inWorld
    );

#ifdef SKR_EXTERNAL_LOBBY
    // Diagnostic: testăm FPS fără condiția SA-MP.
    if (!drawn && skr::intro::IsFinished()) {
        skr::render::fps::Draw(device);
    }
#endif

    HRESULT result = next(
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
    g_hookState.store(skr::bootstrap::ComponentState::WAITING, std::memory_order_release);
    auto** ppDevice = reinterpret_cast<IDirect3DDevice9**>(kGtaDevicePtr);
    for (int i = 0; i < 600 && !g_hookShutdown.load(std::memory_order_acquire); ++i) {
        if (*ppDevice != nullptr) break;
        Sleep(50);
    }

    IDirect3DDevice9* device = *ppDevice;
    if (!device) {
        g_hookState.store(skr::bootstrap::ComponentState::FAILED, std::memory_order_release);
        return 1;
    }

    g_hookedVtable = *reinterpret_cast<void***>(device);
#ifdef SKR_EXTERNAL_LOBBY
    skr::render::fps::Initialize();
#endif

    bool ok = true;
#ifdef SKR_EXTERNAL_LOBBY
    {
        present_chain::SetFrameHandler(&PresentFrame);
        const auto r = present_chain::Install(g_hookedVtable, present_chain::Primary);
        char msg[120]{};
        sprintf_s(msg, "Present hook (primary vtable %p): %s", static_cast<void*>(g_hookedVtable),
                  present_chain::ToString(r));
        fps::Trace(msg);
        ok &= r == present_chain::InstallResult::Installed;
    }
#endif
    EndSceneFn initialEndScene = nullptr;
    ResetFn initialReset = nullptr;
    const bool endSceneOk = PatchVtable(g_hookedVtable, kVtableEndScene,
                                        reinterpret_cast<void*>(&HookedEndScene),
                                        reinterpret_cast<void**>(&initialEndScene));
    if (endSceneOk)
        g_origEndScene.store(initialEndScene, std::memory_order_release);
    const bool resetOk = PatchVtable(g_hookedVtable, kVtableReset,
                                     reinterpret_cast<void*>(&HookedReset),
                                     reinterpret_cast<void**>(&initialReset));
    if (resetOk)
        g_origReset.store(initialReset, std::memory_order_release);
    ok &= endSceneOk && resetOk;
    if (!ok) {
        auto restore = [vtable = g_hookedVtable](int index, void* hook, void* original) {
            if (!original || vtable[index] != hook) return;
            DWORD old = 0;
            if (VirtualProtect(&vtable[index], sizeof(void*), PAGE_READWRITE, &old)) {
                vtable[index] = original;
                VirtualProtect(&vtable[index], sizeof(void*), old, &old);
            }
        };
        if (endSceneOk)
            restore(kVtableEndScene, reinterpret_cast<void*>(&HookedEndScene),
                    reinterpret_cast<void*>(initialEndScene));
        if (resetOk)
            restore(kVtableReset, reinterpret_cast<void*>(&HookedReset),
                    reinterpret_cast<void*>(initialReset));
        g_origEndScene.store(nullptr, std::memory_order_release);
        g_origReset.store(nullptr, std::memory_order_release);
    }

    g_hookInstalled.store(ok);
    g_hookState.store(ok ? skr::bootstrap::ComponentState::INSTALLED
                         : skr::bootstrap::ComponentState::FAILED,
                      std::memory_order_release);
    fps::Trace(ok ? "D3D9 vtable hooks installed" : "D3D9 vtable hook install failed");
    if (!ok) return 1;

    // ── Phase 2: One-shot re-chain after SA-MP world initialization ────
    // SA-MP 0.3.7 R5 overwrites the D3D9 EndScene vtable entry during its
    // own hook setup, which happens around the time the player enters the
    // world (game state 5). When SA-MP obtains its "original" EndScene from
    // a clean temporary device rather than from the live vtable, our hook is
    // silently discarded. We wait for the world transition, then re-patch
    // exactly once — saving SA-MP's hook as our new g_origEndScene so the
    // full chain is preserved: SKR → SA-MP → real EndScene.
    //
    // This is NOT a polling loop: the thread exits after a single re-chain.

    // Wait up to 10 minutes for player to enter the world
    for (int i = 0; i < 6000 && !g_hookShutdown.load(std::memory_order_acquire); ++i) {
        if (IsPlayerInGame()) break;
        Sleep(100);
    }

    if (g_hookShutdown.load(std::memory_order_acquire) || !IsPlayerInGame()) {
        fps::Trace("D3D9 re-chain: shutdown or world never reached, skipping");
        return 0;
    }

    // Let SA-MP finish its hook setup — its initialization continues
    // briefly after the game state reaches 5.
    Sleep(1000);

    if (g_hookShutdown.load(std::memory_order_acquire)) return 0;

    // Re-read the device pointer (may have changed if SA-MP uses a proxy)
    device = *ppDevice;
    if (!device) {
        fps::Trace("D3D9 re-chain: device pointer is null after world init");
        return 0;
    }

    void** vtableNow = *reinterpret_cast<void***>(device);

    {
        char msg[300]{};
        sprintf_s(msg,
            "D3D9 re-chain check: device=%p vtable_before=%p vtable_now=%p "
            "es_entry=%p ours=%p reset_entry=%p ours_reset=%p",
            device, g_hookedVtable, vtableNow,
            vtableNow[kVtableEndScene],
            reinterpret_cast<void*>(&HookedEndScene),
            vtableNow[kVtableReset],
            reinterpret_cast<void*>(&HookedReset));
        fps::Trace(msg);
    }

    bool rechained = false;

    // Re-chain EndScene if it was replaced
    if (vtableNow[kVtableEndScene] != reinterpret_cast<void*>(&HookedEndScene)) {
        EndSceneFn sampEndScene = nullptr;
        if (vtableNow[kVtableEndScene] != nullptr &&
            PatchVtable(vtableNow, kVtableEndScene,
                        reinterpret_cast<void*>(&HookedEndScene),
                        reinterpret_cast<void**>(&sampEndScene))) {
            g_origEndScene.store(sampEndScene, std::memory_order_release);
            rechained = true;
            char msg[200]{};
            sprintf_s(msg,
                "D3D9 re-chain: EndScene restored (samp_es=%p now our orig)",
                reinterpret_cast<void*>(sampEndScene));
            fps::Trace(msg);
        } else {
            fps::Trace("D3D9 re-chain: EndScene VirtualProtect failed");
        }
    } else {
        fps::Trace("D3D9 re-chain: EndScene still intact, no action needed");
    }

    // Re-chain Reset if it was replaced
    if (vtableNow[kVtableReset] != reinterpret_cast<void*>(&HookedReset)) {
        ResetFn sampReset = nullptr;
        if (vtableNow[kVtableReset] != nullptr &&
            PatchVtable(vtableNow, kVtableReset,
                        reinterpret_cast<void*>(&HookedReset),
                        reinterpret_cast<void**>(&sampReset))) {
            g_origReset.store(sampReset, std::memory_order_release);
            rechained = true;
            char msg[200]{};
            sprintf_s(msg,
                "D3D9 re-chain: Reset restored (samp_reset=%p now our orig)",
                reinterpret_cast<void*>(sampReset));
            fps::Trace(msg);
        } else {
            fps::Trace("D3D9 re-chain: Reset VirtualProtect failed");
        }
    }

#ifdef SKR_EXTERNAL_LOBBY
    // Re-chain Present: without it the launch cover never completes on the
    // proxy device and the launcher never receives visual-ready.
    {
        void* entry = vtableNow[present_chain::kPresentIndex];
        const auto r = present_chain::Install(vtableNow, present_chain::Rechained);
        char msg[200]{};
        sprintf_s(msg, "D3D9 re-chain: Present %s (entry=%p, now our orig=%p)",
                  present_chain::ToString(r), entry,
                  reinterpret_cast<void*>(present_chain::Original(present_chain::Rechained)));
        fps::Trace(msg);
        rechained |= r == present_chain::InstallResult::Installed;
    }
#endif

    // Update the stored vtable pointer (may differ if device changed)
    g_hookedVtable = vtableNow;

    fps::Trace(rechained
        ? "D3D9 re-chain COMPLETE — hooks restored after SA-MP init"
        : "D3D9 re-chain: all hooks were intact, no changes made");
    return 0;
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
    skr::native_pause::OnBeforeReset();
    fps::BeforeReset(); skr::launch_cover::BeforeReset(); skr::intro::BeforeReset();
}
extern "C" __declspec(dllexport) BOOL __cdecl SkrEnhancedConfirmedV1() {
    return skr::lobby::IsSkrSession() ? TRUE : FALSE;
}

bool InstallD3D9Hooks() {
    if (g_hookThread) return false;
    g_hookShutdown.store(false, std::memory_order_release);
    g_hookState.store(skr::bootstrap::ComponentState::WAITING, std::memory_order_release);
    g_hookThread = CreateThread(nullptr, 0, D3D9HookThread, nullptr, 0, nullptr);
    if (!g_hookThread)
        g_hookState.store(skr::bootstrap::ComponentState::FAILED, std::memory_order_release);
    return g_hookThread != nullptr;
}

skr::bootstrap::ComponentState GetD3D9HookState() noexcept {
    return g_hookState.load(std::memory_order_acquire);
}

void RemoveD3D9Hooks() {
    g_hookShutdown.store(true, std::memory_order_release);
    if (g_hookThread) {
        WaitForSingleObject(g_hookThread, 5000);
        CloseHandle(g_hookThread);
        g_hookThread = nullptr;
    }
    skr::render::fps::Shutdown();
    skr::intro::Shutdown();

    present_chain::Uninstall();
    if (g_hookedVtable) {
        EndSceneFn endScene = g_origEndScene.load(std::memory_order_acquire);
        ResetFn reset = g_origReset.load(std::memory_order_acquire);
        if (endScene && g_hookedVtable[kVtableEndScene] == reinterpret_cast<void*>(&HookedEndScene)) {
            DWORD old = 0;
            void* entry = &g_hookedVtable[kVtableEndScene];
            VirtualProtect(entry, sizeof(void*), PAGE_READWRITE, &old);
            g_hookedVtable[kVtableEndScene] = reinterpret_cast<void*>(endScene);
            VirtualProtect(entry, sizeof(void*), old, &old);
        }
        if (reset && g_hookedVtable[kVtableReset] == reinterpret_cast<void*>(&HookedReset)) {
            DWORD old = 0;
            void* entry = &g_hookedVtable[kVtableReset];
            VirtualProtect(entry, sizeof(void*), PAGE_READWRITE, &old);
            g_hookedVtable[kVtableReset] = reinterpret_cast<void*>(reset);
            VirtualProtect(entry, sizeof(void*), old, &old);
        }
    }

    g_origEndScene.store(nullptr, std::memory_order_release);
    g_origReset.store(nullptr, std::memory_order_release);
    g_hookedVtable = nullptr;
    g_hookInstalled.store(false);
    g_hookState.store(skr::bootstrap::ComponentState::NOT_STARTED, std::memory_order_release);
}

} // namespace skr::render
