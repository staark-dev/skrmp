#include "lobby_window.h"
#include "game_launcher.h"
#include "ipc/handoff_trace.h"
#include "lobby/settings_io.h"
#include "lobby/public_directory.h"
#include "lobby/server_query.h"
#include "performance/fps_settings.h"
#include <objbase.h>
#include <WebView2.h>
#include <wrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <winhttp.h>
#include <algorithm>
#include <future>
#include <memory>
#include <chrono>
#include <cstdio>
#include <cstdlib>

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Callback;
namespace skr::launcher {
namespace {
constexpr wchar_t kPage[] = L"https://skr-launcher.invalid/lobby.html";
std::wstring Wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(n, 0);
    if (n) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}
std::string Utf8(const wchar_t* s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    std::string out(n, 0);
    if (n) WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
    if (!out.empty()) out.pop_back();
    return out;
}
struct Refresh { std::string servers, info; bool ok = false; };
struct Started { LaunchResult result; DWORD pid; HANDLE process; };

// Only the server opened in Details is queried. Never bulk-query the public list.
bool QueryAddressValid(const std::string& value) {
    if (value.empty() || value.size() > 260) return false;
    const auto colon = value.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 == value.size()) return false;
    const auto host = value.substr(0, colon);
    if (host.size() > 253 || host.find("..") != std::string::npos || host.front() == '-' || host.back() == '-') return false;
    if (!std::all_of(host.begin(), host.end(), [](unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
               (ch >= '0' && ch <= '9') || ch == '.' || ch == '-';
    })) return false;
    const auto port = value.substr(colon + 1);
    if (port.size() > 5 || !std::all_of(port.begin(), port.end(), [](unsigned char ch) { return ch >= '0' && ch <= '9'; })) return false;
    const unsigned number = static_cast<unsigned>(std::strtoul(port.c_str(), nullptr, 10));
    return number >= 1 && number <= 65535;
}
struct Window {
    HWND hwnd{};
    bool done = false;
    LobbyResult result{true};
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> web;
    std::future<Refresh> refresh;
    std::future<lobby::ServerInfo> detailsQuery;
    std::string queuedDetailsAddress;
    // Only visible browser rows. Never scan the complete public server directory.
    std::future<lobby::ServerInfo> visiblePingQuery;
    std::string activePingAddress;
    std::vector<std::string> queuedVisiblePings;
    LaunchConfig config;
    bool canLaunch = false, launching = false, handedOver = false, sessionReady = false, cancelling = false;
    std::future<Started> launch;
    HANDLE process = nullptr, frame = nullptr, takeover = nullptr, ready = nullptr;
    DWORD pid = 0;
    ULONGLONG deadline = 0;
    ~Window() {
        if (process) CloseHandle(process);
        if (frame) CloseHandle(frame);
        if (takeover) CloseHandle(takeover);
        if (ready) CloseHandle(ready);
    }
    void ReturnToLobby(const std::string& error = {}) {
        if (process) { CloseHandle(process); process = nullptr; }
        launching = handedOver = sessionReady = cancelling = false;
        ipc::TraceHandoff("lobby-restored");
        SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        ShowWindow(hwnd, SW_SHOW);
        SetForegroundWindow(hwnd);
        Emit("skr:launch:idle", error);
    }
    void CancelLaunch() {
        cancelling = true;
        // Only our own child is stopped; cleanup completes on the UI timer.
        if (launch.valid()) { deadline = 1; return; }
        if (process) TerminateProcess(process, 1);
    }
    void Start(const std::string& host, uint16_t port, const std::string& nickname) {
        if (launching || process) return;
        if (!frame) frame = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!takeover) takeover = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ready) ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!frame || !takeover || !ready) { Emit("skr:launch:idle", "Nu se poate pregăti comunicarea cu jocul."); return; }
        ResetEvent(frame); ResetEvent(takeover); ResetEvent(ready);
        config.serverAddress = host; config.serverPort = port; config.playerName = Wide(nickname);
        config.frameReady = frame; config.takeover = takeover; config.sessionReady = ready;
        launching = true; handedOver = sessionReady = cancelling = false;
        ipc::TraceHandoff("launch-started");
        deadline = GetTickCount64() + 120000;
        Emit("skr:launch:loading");
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        launch = std::async(std::launch::async, [c = config]() mutable {
            const auto discovery = lobby::QueryServer(c.serverAddress + ":" + std::to_string(c.serverPort), 1500, true);
            c.enhancedAdvertised = discovery.rulesKnown && discovery.skrClient == "2";
            Started r{LaunchResult::ProcessCreateFailed, 0, nullptr};
            r.result = LaunchGame(c, r.pid, r.process);
            return r;
        });
    }
    void TickLaunch() {
        if (launch.valid() && launch.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            try {
                auto r = launch.get(); process = r.process; pid = r.pid;
                if (r.result != LaunchResult::Success) { ReturnToLobby(LaunchResultToString(r.result)); return; }
            } catch (...) { ReturnToLobby("Lansarea a eșuat."); return; }
        }
        if (!process) return;
        if (cancelling) TerminateProcess(process, 1);
        if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0) {
            ReturnToLobby(sessionReady ? "" : "Jocul s-a închis înainte de finalizarea încărcării."); return;
        }
        if (!handedOver && WaitForSingleObject(frame, 0) == WAIT_OBJECT_0) {
            // Child signalled only after a successful Present of the SKR cover.
            // Seamless handoff: bring GTA's window above the launcher BEFORE
            // hiding it, so the desktop is never exposed between the two.
            handedOver = true;
            struct FindCtx { DWORD pid; HWND result; };
            FindCtx ctx{pid, nullptr};
            EnumWindows([](HWND wnd, LPARAM lp) -> BOOL {
                auto* c = reinterpret_cast<FindCtx*>(lp);
                DWORD owner = 0; GetWindowThreadProcessId(wnd, &owner);
                if (owner == c->pid && IsWindowVisible(wnd) && !GetWindow(wnd, GW_OWNER)) {
                    c->result = wnd; return FALSE;
                }
                return TRUE;
            }, reinterpret_cast<LPARAM>(&ctx));
            if (ctx.result) {
                // GTA on top while launcher still covers the screen behind it.
                SetWindowPos(ctx.result, HWND_TOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                SetForegroundWindow(ctx.result);
                ipc::TraceHandoff("handoff-gta-foreground");
            }
            // Hide launcher — GTA is already covering the screen.
            SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            ShowWindow(hwnd, SW_HIDE);
            // Remove TOPMOST from GTA so Alt+Tab and other windows work normally.
            if (ctx.result) {
                SetWindowPos(ctx.result, HWND_NOTOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
            SetEvent(takeover);
            ipc::TraceHandoff("frame-confirmed-window-hidden");
        }
        if (!sessionReady && WaitForSingleObject(ready, 0) == WAIT_OBJECT_0) {
            sessionReady = true; ipc::TraceHandoff("session-visual-ready");
        }
        if (!sessionReady && GetTickCount64() >= deadline) {
            TerminateProcess(process, 1);
            ipc::TraceHandoff("handoff-timeout");
            // Keep monitoring until the child has actually exited before another launch.
            deadline = GetTickCount64() + 10000;
            SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            ShowWindow(hwnd, SW_SHOW);
            Emit("skr:launch:failed", "GTA sau interfața serverului nu a confirmat încărcarea. Revenim în lobby.");
        }
    }
    void Fail(const wchar_t* message) {
        if (done) return;
        MessageBoxW(hwnd, message, L"SKR Launcher experimental", MB_OK | MB_ICONERROR);
        done = true;
    }
    void Emit(const std::string& event, const std::string& data = {}) {
        if (web && !done) web->PostWebMessageAsString(Wide(event + "\n" + data).c_str());
    }
    void Personal() {
        std::vector<lobby::PersonalServer> list;
        lobby::LoadPersonalServers(list);
        Emit("skr:lobby:servers:personal", lobby::PersonalServersToJson(list));
    }
    void RefreshServers() {
        Personal();
        if (refresh.valid()) return;
        refresh = std::async(std::launch::async, [] {
            Refresh r;
            std::vector<lobby::PublicServer> pub;
            std::string error;
            r.ok = lobby::FetchOpenMpServers(pub, error);
            r.servers = lobby::PublicServersToLobbyJson(pub);
            std::vector<lobby::PersonalServer> personal;
            lobby::LoadPersonalServers(personal);
            std::vector<lobby::ServerInfo> info;
            for (const auto& server : personal) info.push_back(lobby::QueryServer(server.address, 300));
            r.info = lobby::ServerInfoToJson(info);
            return r;
        });
    }
    void QueryDetails(const std::string& address) {
        if (!QueryAddressValid(address) || launching || done) return;
        if (detailsQuery.valid()) { queuedDetailsAddress = address; return; }
        // 400ms per UDP request (`i`, `r`); never block the WebView2 thread.
        detailsQuery = std::async(std::launch::async, [address] { return lobby::QueryServer(address, 400); });
    }

    void StartNextVisiblePing() {
        if (done || launching || visiblePingQuery.valid() || queuedVisiblePings.empty()) return;
        activePingAddress = queuedVisiblePings.front();
        queuedVisiblePings.erase(queuedVisiblePings.begin());
        // One UDP 'i' query, 250ms timeout. No 'r'/rules query for list latency.
        visiblePingQuery = std::async(std::launch::async, [address = activePingAddress] {
            return lobby::QueryServer(address, 250, false);
        });
    }
    void QueueVisiblePings(const std::string& payload) {
        if (launching || done || payload.size() > 4096) return;
        std::vector<std::string> next;
        size_t start = 0;
        while (start < payload.size() && next.size() < 12) {
            const size_t end = payload.find(';', start);
            const std::string address = payload.substr(start, end == std::string::npos ?
                std::string::npos : end - start);
            if (QueryAddressValid(address) && address != activePingAddress &&
                std::find(next.begin(), next.end(), address) == next.end())
                next.push_back(address);
            if (end == std::string::npos) break;
            start = end + 1;
        }
        // Replace stale scroll requests rather than accumulating a 1,200-server queue.
        queuedVisiblePings = std::move(next);
        StartNextVisiblePing();
    }
    void Message(const std::string& message) {
        if (message.size() > 4096 || done) return;
        auto split = message.find('\n');
        if (split == std::string::npos) return;
        auto event = message.substr(0, split), data = message.substr(split + 1);
        if (event == "skr:external:open") {
            auto url = Wide(data);
            URL_COMPONENTS parts{};
            parts.dwStructSize = sizeof(parts);
            parts.dwHostNameLength = parts.dwUserNameLength = parts.dwPasswordLength = static_cast<DWORD>(-1);
            bool valid = !url.empty() && url.size() <= 2048 &&
                url.find_first_of(L"\r\n\t\\") == std::wstring::npos &&
                std::none_of(url.begin(), url.end(), [](wchar_t c) { return c <= 32 || c == 127; }) &&
                WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts) &&
                (parts.nScheme == INTERNET_SCHEME_HTTP || parts.nScheme == INTERNET_SCHEME_HTTPS) &&
                parts.dwHostNameLength && !parts.dwUserNameLength && !parts.dwPasswordLength;
            if (!valid) { Emit("skr:external:error", "Invalid website address."); return; }
            const auto openResult = reinterpret_cast<INT_PTR>(
                ShellExecuteW(hwnd, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
            Emit(openResult > 32 ? "skr:external:opened" : "skr:external:error",
                 openResult > 32 ? "" : "Windows could not open the default browser.");
            return;
        }
        if (event == "skr:fps:get") {
            Emit("skr:fps:ready", performance::FpsSettingsToJson(performance::LoadFpsSettings()));
            return;
        }
        if (event == "skr:fps:save") {
            unsigned show = 0, cap = 0;
            char extra = 0;
            if (sscanf_s(data.c_str(), "show=%u;cap=%u%c", &show, &cap, &extra, 1) != 2 ||
                show > 1 || !performance::IsValidCap(cap)) {
                Emit("skr:fps:error", "Invalid FPS preferences.");
                return;
            }
            performance::FpsSettings settings{show != 0, cap};
            if (!performance::SaveFpsSettings(settings)) Emit("skr:fps:error", "Could not save FPS preferences.");
            else Emit("skr:fps:saved", performance::FpsSettingsToJson(settings));
            return;
        }
        if (event == "skr:launch:cancel") { CancelLaunch(); return; }
        if (event == "skr:lobby:servers:query") { QueryDetails(data); return; }
        if (event == "skr:lobby:servers:ping") { QueueVisiblePings(data); return; }
        if (launching) return;
        lobby::Settings settings;
        lobby::LoadSettings(settings);
        if (event == "skr:lobby:quit") { done = true; return; }
        if (event == "skr:lobby:settings:get") Emit("skr:lobby:settings:ready", lobby::SettingsToJson(settings));
        else if (event == "skr:lobby:settings:save") {
            if (!lobby::SettingsFromJson(data, settings)) Emit("skr:lobby:settings:error", "invalid");
            else if (!lobby::SaveSettings(settings)) Emit("skr:lobby:settings:error", "write");
            else Emit("skr:lobby:settings:saved", lobby::SettingsToJson(settings));
        } else if (event == "skr:lobby:nickname:save") {
            if (!lobby::NicknameFromJson(data, settings.nickname)) Emit("skr:lobby:nickname:error", "invalid");
            else if (!lobby::SaveSettings(settings)) Emit("skr:lobby:nickname:error", "write");
            else Emit("skr:lobby:nickname:saved", settings.nickname);
        } else if (event == "skr:lobby:connect" || event == "skr:lobby:play") {
            uint16_t port = 0;
            std::string host, nickname;
            if (!lobby::ConnectRequestFromJson(data, host, port, nickname)) Emit("skr:lobby:connect:error", "invalid");
            else {
                if (canLaunch) { Start(host, port, nickname); return; }
                result = {false, host + ":" + std::to_string(port), nickname};
                done = true;
            }
        } else if (event == "skr:lobby:servers:request") RefreshServers();
        else if (event == "skr:lobby:servers:add" || event == "skr:lobby:servers:remove") {
            lobby::PersonalServer server;
            std::vector<lobby::PersonalServer> list;
            if (!lobby::PersonalServerFromJson(data, server)) { Emit("skr:lobby:servers:error", "invalid"); return; }
            lobby::LoadPersonalServers(list);
            auto it = std::find_if(list.begin(), list.end(), [&](const auto& s) { return s.address == server.address; });
            if (event == "skr:lobby:servers:remove") { if (it != list.end()) list.erase(it); }
            else if (it != list.end()) *it = server;
            else if (list.size() >= lobby::kMaxPersonalServers) { Emit("skr:lobby:servers:error", "limit"); return; }
            else list.push_back(server);
            if (!lobby::SavePersonalServers(list)) Emit("skr:lobby:servers:error", "write");
            else RefreshServers();
        }
    }
};
LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* state = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        state = static_cast<Window*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (state) {
        if (msg == WM_CLOSE) {
            if (state->launching) state->CancelLaunch(); else state->done = true;
            return 0;
        }
        if (msg == WM_TIMER) state->TickLaunch();
        if (msg == WM_SIZE && state->controller) {
            RECT rect; GetClientRect(hwnd, &rect); state->controller->put_Bounds(rect);
        }
        if (msg == WM_TIMER && state->visiblePingQuery.valid() &&
            state->visiblePingQuery.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            try {
                auto info = state->visiblePingQuery.get();
                state->Emit("skr:lobby:servers:ping", lobby::ServerInfoToJson({info}));
            } catch (...) { /* Query timeout/failure; preserve unknown ping. */ }
            state->activePingAddress.clear();
            state->StartNextVisiblePing();
        }
        if (msg == WM_TIMER && state->detailsQuery.valid() &&
            state->detailsQuery.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            try {
                auto info = state->detailsQuery.get();
                state->Emit("skr:lobby:servers:info", lobby::ServerInfoToJson({info}));
            } catch (...) { /* Query failed; the next details visit can retry. */ }
            if (!state->queuedDetailsAddress.empty()) {
                auto address = std::move(state->queuedDetailsAddress);
                state->queuedDetailsAddress.clear();
                state->QueryDetails(address);
            }
        }
        if (msg == WM_TIMER && state->refresh.valid() &&
            state->refresh.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            try {
                auto r = state->refresh.get();
                state->Emit("skr:lobby:servers", r.servers);
                state->Emit("skr:lobby:servers:info", r.info);
                if (!r.ok) state->Emit("skr:lobby:servers:error", "directory");
            } catch (...) { state->Emit("skr:lobby:servers:error", "directory"); }
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
}
LobbyResult ShowLobbyWindow(HINSTANCE instance, const std::wstring& lobbyDir, bool smoke, const LaunchConfig* launchConfig) {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return {true};
    auto state = std::make_shared<Window>();
    if (launchConfig) { state->config = *launchConfig; state->canLaunch = true; }
    WNDCLASSW wc{};
    wc.lpfnWndProc = Proc; wc.hInstance = instance; wc.lpszClassName = L"SKRExternalLobby";
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    RegisterClassW(&wc);
    state->hwnd = CreateWindowExW(0, wc.lpszClassName, L"SKR Launcher experimental", WS_POPUP,
        0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), nullptr, nullptr, instance, state.get());
    if (!state->hwnd) { CoUninitialize(); return {true}; }
    ShowWindow(state->hwnd, SW_SHOW);
    SetTimer(state->hwnd, 1, 100, nullptr);
    PWSTR local = nullptr;
    std::wstring profile;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) {
        profile = std::wstring(local) + L"\\SKR\\WebView2"; CoTaskMemFree(local);
    }
    HRESULT hr = profile.empty() ? E_FAIL : CreateCoreWebView2EnvironmentWithOptions(nullptr, profile.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([state, lobbyDir, smoke](HRESULT error, ICoreWebView2Environment* env) -> HRESULT {
            if (state->done) return S_OK;
            if (FAILED(error) || !env) { state->Fail(L"WebView2 Runtime indisponibil. Instalează Microsoft Edge WebView2 Runtime."); return S_OK; }
            HRESULT created = env->CreateCoreWebView2Controller(state->hwnd,
                Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>([state, lobbyDir, smoke](HRESULT err, ICoreWebView2Controller* controller) -> HRESULT {
                    if (state->done) return S_OK;
                    if (FAILED(err) || !controller) { state->Fail(L"Nu se poate crea fereastra WebView2."); return S_OK; }
                    state->controller = controller;
                    controller->get_CoreWebView2(&state->web);
                    ComPtr<ICoreWebView2_3> mapping;
                    if (!state->web || FAILED(state->web.As(&mapping)) || FAILED(mapping->SetVirtualHostNameToFolderMapping(
                        L"skr-launcher.invalid", lobbyDir.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS))) {
                        state->Fail(L"Nu se poate încărca interfața locală."); return S_OK;
                    }
                    EventRegistrationToken token;
                    auto* raw = state.get();
                    if (state->config.localHandoffTest) state->web->add_NavigationCompleted(
                        Callback<ICoreWebView2NavigationCompletedEventHandler>(
                        [weak = std::weak_ptr<Window>(state)](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                            auto keep = weak.lock(); BOOL ok = FALSE; args->get_IsSuccess(&ok);
                            if (keep && ok && keep->config.localHandoffTest) {
                                keep->config.localHandoffTest = false;
                                keep->Message("skr:lobby:connect\n{\"address\":\"127.0.0.1:7777\",\"nickname\":\"SKR_Test\"}");
                            }
                            return S_OK;
                        }).Get(), &token);
                    if (smoke) state->web->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>(
                        [weak = std::weak_ptr<Window>(state)](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                            auto keep = weak.lock();
                            if (!keep || keep->done) return S_OK;
                            auto* raw = keep.get();
                            BOOL ok = FALSE; args->get_IsSuccess(&ok);
                            if (!ok) { raw->done = true; return S_OK; }
                            HRESULT executed = raw->web->ExecuteScript(
                                LR"(!!(window.skrLobby && window.cef && window.chrome.webview && document.getElementById('splash') && document.styleSheets.length && document.body.clientWidth > 0))",
                                Callback<ICoreWebView2ExecuteScriptCompletedHandler>([keep](HRESULT error, LPCWSTR json) -> HRESULT {
                                    auto* raw = keep.get();
                                    raw->result.smokePassed = SUCCEEDED(error) && json && wcscmp(json, L"true") == 0;
                                    raw->done = true;
                                    return S_OK;
                                }).Get());
                            if (FAILED(executed)) raw->done = true;
                            return S_OK;
                        }).Get(), &token);
                    state->web->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
                        [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                            LPWSTR uri = nullptr; args->get_Uri(&uri);
                            if (!uri || wcscmp(uri, kPage)) args->put_Cancel(TRUE);
                            CoTaskMemFree(uri); return S_OK;
                        }).Get(), &token);
                    state->web->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                        [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT { args->put_Handled(TRUE); return S_OK; }).Get(), &token);
                    state->web->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                        [raw](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                            LPWSTR source = nullptr, message = nullptr;
                            args->get_Source(&source);
                            if (source && wcscmp(source, kPage) == 0 && SUCCEEDED(args->TryGetWebMessageAsString(&message)) && message)
                                raw->Message(Utf8(message));
                            CoTaskMemFree(source); CoTaskMemFree(message); return S_OK;
                        }).Get(), &token);
                    RECT rect; GetClientRect(state->hwnd, &rect); controller->put_Bounds(rect);
                    if (FAILED(state->web->Navigate(kPage))) state->Fail(L"Navigarea către lobby a eșuat.");
                    return S_OK;
                }).Get());
            if (FAILED(created)) state->Fail(L"Inițializarea WebView2 a eșuat.");
            return S_OK;
        }).Get());
    if (FAILED(hr)) state->Fail(L"WebView2 nu poate porni. Verifică instalarea Microsoft Edge WebView2 Runtime.");
    MSG msg;
    while (!state->done) {
        int status = GetMessageW(&msg, nullptr, 0, 0);
        if (status <= 0) break;
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    state->done = true;
    KillTimer(state->hwnd, 1);
    if (state->controller) state->controller->Close();
    state->web.Reset(); state->controller.Reset();
    DestroyWindow(state->hwnd);
    auto result = state->result;
    state.reset();
    CoUninitialize();
    return result;
}
} // namespace skr::launcher
