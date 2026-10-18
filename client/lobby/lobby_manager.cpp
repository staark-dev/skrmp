#include "lobby/lobby_manager.h"
#include "lobby/menu_patch.h"
#include "lobby/settings_io.h"
#include "cef-bridge/cef_bridge.h"
#include "intro/urban_intro.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>

namespace skr::lobby {

namespace {

// ── Logging ────────────────────────────────────────────────────────
std::mutex g_logMutex;
std::wstring LogDir() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&LogDir), &self);
    wchar_t p[MAX_PATH];
    DWORD n = GetModuleFileNameW(self, p, MAX_PATH);
    if (!n || n >= MAX_PATH) return {};
    std::wstring s(p, n);
    size_t slash = s.find_last_of(L'\\');
    if (slash == std::wstring::npos) return {};
    return s.substr(0, slash + 1) + L"SKR\\";
}
void Log(const char* fmt, ...) {
    std::wstring dir = LogDir();
    if (dir.empty()) return;
    CreateDirectoryW(dir.c_str(), nullptr);   // tolerate ALREADY_EXISTS
    std::lock_guard<std::mutex> lk(g_logMutex);
    FILE* f = _wfopen((dir + L"lobby.log").c_str(), L"a");
    if (!f) return;
    fprintf(f, "[%lu] ", GetTickCount());
    va_list ap; va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

// ── State ──────────────────────────────────────────────────────────
std::atomic<bool> g_initialized{false};
std::atomic<bool> g_disabled{true};   // default OFF until lobby.on exists (see Initialize)
std::atomic<bool> g_browserCreated{false};
std::atomic<bool> g_browserShown{false};
std::atomic<bool> g_menuPatched{false};
std::atomic<bool> g_eventsSubscribed{false};

constexpr uint32_t kBrowserId = skr::cef::BrowserId::kMainMenu;

// ── Resource URL ───────────────────────────────────────────────────
// Lobby HTML ships alongside the DLL at <dll_dir>\SKR\lobby\lobby.html.
std::string LobbyUrl() {
    std::wstring dir = LogDir();
    if (dir.empty()) return {};
    std::wstring full = dir + L"lobby\\lobby.html";
    // Convert "C:\path\lobby.html" to "file:///C:/path/lobby.html"
    std::wstring url = L"file:///";
    for (wchar_t c : full) url += (c == L'\\' ? L'/' : c);
    int n = WideCharToMultiByte(CP_UTF8, 0, url.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, url.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

// ── CEF event handlers ─────────────────────────────────────────────
// Writes the current launcher.ini into a sidecar JSON next to lobby.html so
// JS can `fetch("settings.json")`. samp-cef runs its browser process with
// --allow-file-access-from-files (verified in cef_sandbox config at commit
// f14fe4fa), which permits same-directory file:// fetches.
void WriteSettingsSidecar() {
    Settings s;
    LoadSettings(s);
    std::wstring dir = LogDir();
    if (dir.empty()) return;
    std::wstring sidecar = dir + L"lobby\\settings.json";
    HANDLE h = CreateFileW(sidecar.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    std::string json = SettingsToJson(s);
    DWORD written = 0;
    WriteFile(h, json.data(), static_cast<DWORD>(json.size()), &written, nullptr);
    CloseHandle(h);
}

int __cdecl OnSettingsGet(const char*, skr::cef::CefListValue*) {
    WriteSettingsSidecar();
    auto& bridge = skr::cef::CefBridge::Instance();
    bridge.EmitEvent("skr:lobby:settings:ready", nullptr);
    Log("settings:get -> sidecar written, ready event emitted");
    return 0;
}

int __cdecl OnSettingsSave(const char*, skr::cef::CefListValue*) {
    // Phase 1: the samp-cef bridge wrapper we embed does not yet expose
    // CefListValue readers, so the JSON string the JS side packs into the
    // event cannot be read on this side. The JS lobby therefore shows a
    // read-only confirmation and tells the user to edit through the WPF
    // launcher until the bridge gains list getters (Phase 2 scope).
    auto& bridge = skr::cef::CefBridge::Instance();
    bridge.EmitEvent("skr:lobby:settings:save-pending", nullptr);
    Log("settings:save -> deferred to Phase 2 (CefListValue readers missing)");
    return 0;
}

int __cdecl OnPlay(const char*, skr::cef::CefListValue*) {
    Log("play -> Phase 2 (connect) not implemented");
    return 0;
}

int __cdecl OnQuit(const char*, skr::cef::CefListValue*) {
    Log("quit requested from lobby");
    // Close the GTA process cleanly. ExitProcess runs atexit handlers in this
    // module; d3d9 teardown is driven by that.
    PostQuitMessage(0);
    // Fallback for GTA's own message loop which does not react to PostQuitMessage
    // when stuck on the vanilla menu; schedule a hard exit shortly after.
    HWND w = FindWindowW(L"Grand theft auto San Andreas", nullptr);
    if (w) PostMessageW(w, WM_CLOSE, 0, 0);
    ExitProcess(0);
}

void SubscribeOnce() {
    if (g_eventsSubscribed.exchange(true)) return;
    auto& bridge = skr::cef::CefBridge::Instance();
    bridge.Subscribe("skr:lobby:settings:get",  &OnSettingsGet);
    bridge.Subscribe("skr:lobby:settings:save", &OnSettingsSave);
    bridge.Subscribe("skr:lobby:play",          &OnPlay);
    bridge.Subscribe("skr:lobby:quit",          &OnQuit);
    Log("CEF event handlers subscribed");
}

// ── Full-screen black cover (hides any menu pixels under the CEF page) ─
void DrawCover(IDirect3DDevice9* device) {
    if (!device) return;
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
        device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
        device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_LIGHTING, FALSE);
        device->SetTexture(0, nullptr);
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        struct V { float x, y, z, rhw; DWORD c; };
        float w = static_cast<float>(d.Width), h = static_cast<float>(d.Height);
        DWORD black = D3DCOLOR_ARGB(255, 0, 0, 0);
        V quad[4] = {
            {0, 0, 0, 1, black}, {w, 0, 0, 1, black},
            {0, h, 0, 1, black}, {w, h, 0, 1, black},
        };
        device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));
    }
    sb->Apply();
    sb->Release();
}

} // namespace

void Initialize() {
    if (g_initialized.exchange(true)) return;
    // Phase 1 kill switch: the in-game lobby replaces the GTA main menu but
    // the live test showed input/visibility issues we haven't solved yet, so
    // the module ships disabled. Drop a file named `SKR\lobby.on` next to
    // SKR.Client.dll to re-enable it for testing — mirrors the `intro.off`
    // convention from urban_intro.cpp.
    std::wstring dir = LogDir();
    bool enabled = false;
    if (!dir.empty()) {
        enabled = GetFileAttributesW((dir + L"lobby.on").c_str()) != INVALID_FILE_ATTRIBUTES;
    }
    g_disabled.store(!enabled);
    Log("lobby manager initialize (lobby.on %s -> %s)",
        enabled ? "present" : "missing",
        enabled ? "ENABLED" : "DISABLED (vanilla GTA menu will show)");
}

void Shutdown() {
    if (!g_initialized.exchange(false)) return;
    if (g_menuPatched.exchange(false)) RestoreGtaMainMenu();
    if (g_browserCreated.load()) {
        auto& bridge = skr::cef::CefBridge::Instance();
        if (bridge.IsReady()) bridge.DestroyBrowser(kBrowserId);
    }
    Log("lobby manager shutdown");
}

void OnEndSceneAfterIntro(IDirect3DDevice9* device) {
    if (g_disabled.load()) return;   // vanilla menu path: do nothing
    // One-shot "we got here" marker so the next failure mode is distinguishable
    // from "post-intro frame never fires" in lobby.log.
    static std::atomic<bool> firstFrameLogged{false};
    if (!firstFrameLogged.exchange(true)) Log("first post-intro EndScene frame");

    // Patch the vanilla menu now; retried each frame until it sticks. On
    // success/failure we log the actual first byte seen so the next run tells
    // us exactly why if the offset ever drifts.
    if (!g_menuPatched.load()) {
        MenuPatchStatus st{};
        bool ok = PatchGtaMainMenu(st);
        static int lastReportedByte0 = -1, lastReportedByte1 = -1;
        if (ok) {
            g_menuPatched.store(true);
            Log("CMenuManager Process/DrawFrontEnd patched: %s first=0x%02X %s %s first=0x%02X %s",
                st.entries[0].name, st.entries[0].firstByte,
                st.entries[0].alreadyPatched ? "(already 0xC3)" : "(we wrote 0xC3)",
                st.entries[1].name, st.entries[1].firstByte,
                st.entries[1].alreadyPatched ? "(already 0xC3)" : "(we wrote 0xC3)");
        } else {
            // Log once per distinct (byte0, byte1) tuple so a stuck repeat
            // mode doesn't flood the file but a transient change is captured.
            int b0 = st.entries[0].firstByte, b1 = st.entries[1].firstByte;
            if (b0 != lastReportedByte0 || b1 != lastReportedByte1) {
                lastReportedByte0 = b0; lastReportedByte1 = b1;
                Log("menu patch FAILED: %s@0x%08IX first=0x%02X reason=%s | %s@0x%08IX first=0x%02X reason=%s",
                    st.entries[0].name, st.entries[0].addr, b0,
                    st.entries[0].reason ? st.entries[0].reason : "ok",
                    st.entries[1].name, st.entries[1].addr, b1,
                    st.entries[1].reason ? st.entries[1].reason : "ok");
            }
        }
    }

    // samp-cef has no `cef_samp_mainloop` plugin callback (verified by the
    // launcher-ui session against deps/SAMPC-main). Drive the lobby's CEF
    // tick from here instead — EndScene fires on the game thread at frame
    // rate, which is exactly the cadence OnCefTick wants.
    OnCefTick();

    // Draw cover BEFORE SA-MP's UI overlays and the CEF plugin render pass so
    // the lobby HTML (which carries its own background) appears on an opaque
    // backdrop. CEF renders on top of this on the samp-present path.
    DrawCover(device);
}

void OnCefTick() {
    if (g_disabled.load()) return;
    auto& bridge = skr::cef::CefBridge::Instance();
    if (!bridge.IsReady()) return;
    static bool loggedBridgeReady = false;
    if (!loggedBridgeReady) { loggedBridgeReady = true; Log("CefBridge: Ready (api_ installed)"); }
    if (!bridge.IsCefReady()) return;
    static bool loggedCefReady = false;
    if (!loggedCefReady) { loggedCefReady = true; Log("CefBridge: cef_ready() -> true"); }

    SubscribeOnce();

    if (!g_browserCreated.load()) {
        // Wait until the intro has finished before stealing the frame; the
        // intro renders through samp-present and would otherwise overlap.
        if (!skr::intro::IsFinished()) return;
        std::string url = LobbyUrl();
        if (url.empty()) { Log("lobby URL resolution failed"); return; }
        WriteSettingsSidecar();           // ensure settings.json exists before load
        Log("creating lobby browser: %s", url.c_str());
        bridge.CreateBrowser(kBrowserId, url.c_str(), /*hidden*/ false, /*focused*/ true);
        g_browserCreated.store(true);
        return;
    }
    if (!g_browserShown.load() && bridge.BrowserReady(kBrowserId)) {
        bridge.ShowBrowser(kBrowserId);
        bridge.FocusBrowser(kBrowserId);
        g_browserShown.store(true);
        Log("lobby browser shown and focused");
    }
}

bool IsLobbyActive() { return g_browserShown.load(); }
bool IsEnabled()     { return !g_disabled.load(); }

} // namespace skr::lobby
