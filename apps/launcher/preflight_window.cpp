#include "preflight_window.h"
#include "preflight/preflight_controller.h"
#include "preflight/game_installation_detector.h"
#include "preflight/exe_fingerprint.h"
#include "preflight/samp_inspector.h"
#include "preflight/dependency_inspector.h"
#include "preflight/verification_request_guard.h"
#include "lobby/flat_json.h"
#include <objbase.h>
#include <shlobj.h>
#include <shellapi.h>
#include <WebView2.h>
#include <wrl.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <vector>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace skr::launcher {
namespace {

constexpr wchar_t kPage[] = L"https://skr-preflight.invalid/preflight.html";
constexpr wchar_t kWindowClass[] = L"SKREnhancedPreflightWebView";
constexpr int kCompactWidth = 540;
constexpr int kCompactHeight = 560;
constexpr int kExpandedWidth = 740;
constexpr int kExpandedHeight = 610;
constexpr UINT_PTR kTimerId = 1;
constexpr int kAppIconResourceId = 1;

std::string Utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string result(static_cast<size_t>(count), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), result.data(), count, nullptr, nullptr) != count) return {};
    return result;
}

std::wstring Wide(const std::string& value) {
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring result(static_cast<size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), result.data(), count) != count) return {};
    return result;
}

std::string JsonEscape(const std::string& value) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(value.size() + 8);
    for (unsigned char ch : value) {
        switch (ch) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (ch < 0x20) {
                    result += "\\u00";
                    result.push_back(hex[(ch >> 4) & 0x0f]);
                    result.push_back(hex[ch & 0x0f]);
                } else {
                    result.push_back(static_cast<char>(ch));
                }
        }
    }
    return result;
}

std::string Quote(const std::string& value) { return "\"" + JsonEscape(value) + "\""; }
std::string Quote(const std::wstring& value) { return Quote(Utf8(value)); }

const char* StatusKey(preflight::CheckStatus status) {
    switch (status) {
        case preflight::CheckStatus::Verified: return "verified";
        case preflight::CheckStatus::Warning: return "warning";
        case preflight::CheckStatus::ActionRequired: return "action-required";
        case preflight::CheckStatus::Unsupported: return "unsupported";
        case preflight::CheckStatus::Pending: return "pending";
    }
    return "pending";
}

const char* StageKey(preflight::PreflightStage stage) {
    switch (stage) {
        case preflight::PreflightStage::LocatingGta: return "locating-gta";
        case preflight::PreflightStage::VerifyingExe: return "verifying-executable";
        case preflight::PreflightStage::ScanningSamp: return "scanning-samp";
        case preflight::PreflightStage::CheckingDeps: return "checking-dependencies";
        case preflight::PreflightStage::PreparingLauncher: return "validating-runtime";
        case preflight::PreflightStage::Complete: return "complete";
    }
    return "pending";
}

int StageIndex(preflight::PreflightStage stage) {
    if (stage == preflight::PreflightStage::Complete) return preflight::kPreflightStageCount;
    return static_cast<int>(stage) + 1;
}

std::wstring ParentDirectory(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

std::string FolderName(const std::wstring& path) {
    if (path.empty()) return "GTA San Andreas";
    const auto slash = path.find_last_of(L"\\/");
    return Utf8(slash == std::wstring::npos ? path : path.substr(slash + 1));
}

bool SamePath(const std::wstring& a, const std::wstring& b) {
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

bool CandidateMatchesPath(const preflight::GtaCandidate& candidate, const std::wstring& path) {
    if (SamePath(candidate.exePath, path)) return true;
    return std::any_of(candidate.aliases.begin(), candidate.aliases.end(),
        [&](const auto& alias) { return SamePath(alias, path); });
}

std::string CheckJson(const preflight::CheckResult& check) {
    std::ostringstream out;
    out << "{\"status\":" << Quote(StatusKey(check.status))
        << ",\"code\":" << Quote(check.code)
        << ",\"summary\":" << Quote(check.summary)
        << ",\"detail\":" << Quote(check.detail)
        << ",\"remediation\":" << Quote(check.remediation) << "}";
    return out.str();
}

const preflight::CheckResult& PrimaryBlocker(const preflight::PreflightReport& report) {
    const preflight::CheckResult* checks[] = {
        &report.gtaDiscovery, &report.exeVerification, &report.sampInspection,
        &report.depVerification, &report.bundledRuntime
    };
    for (const auto* check : checks) if (preflight::IsBlocking(check->status)) return *check;
    for (const auto* check : checks) if (check->status == preflight::CheckStatus::Warning) return *check;
    return report.exeVerification;
}

bool NativeApproved(const preflight::PreflightReport& report) {
    const preflight::CheckResult* checks[] = {
        &report.gtaDiscovery, &report.exeVerification, &report.sampInspection,
        &report.depVerification, &report.bundledRuntime
    };
    return std::all_of(std::begin(checks), std::end(checks), [](const auto* check) {
        return check->status != preflight::CheckStatus::Pending &&
               !preflight::IsBlocking(check->status);
    });
}

struct VerificationRun {
    uint64_t requestId = 0;
    std::wstring requestedPath;
    preflight::PreflightReport report;
    std::vector<preflight::GtaCandidate> candidates;
};

VerificationRun RunVerification(const std::wstring& launcherDir,
                                const std::wstring& preferredPath,
                                uint64_t requestId,
                                preflight::PreflightProgressCallback progress) {
    VerificationRun run;
    run.requestId = requestId;
    run.requestedPath = preferredPath;
    run.candidates = preflight::DetectGtaInstallations(launcherDir);

    const std::wstring preferredExe = preflight::ResolveGtaExePath(preferredPath);
    if (!preferredExe.empty() && std::none_of(run.candidates.begin(), run.candidates.end(),
            [&](const auto& candidate) { return CandidateMatchesPath(candidate, preferredExe); })) {
        run.candidates.insert(run.candidates.begin(), {preferredExe, ParentDirectory(preferredExe), "selected"});
    }

    if (!preferredExe.empty()) {
        std::vector<preflight::GtaCandidate> selected;
        const auto it = std::find_if(run.candidates.begin(), run.candidates.end(),
            [&](const auto& candidate) { return CandidateMatchesPath(candidate, preferredExe); });
        if (it != run.candidates.end()) selected.push_back(*it);
        run.report.gtaDiscovery = preflight::EvaluateGtaDiscovery(selected,
            run.report.gtaExePath, run.report.gtaDir);
    } else {
        run.report.gtaDiscovery = preflight::EvaluateGtaDiscovery(run.candidates,
            run.report.gtaExePath, run.report.gtaDir);
    }
    if (progress) progress(preflight::PreflightStage::LocatingGta, run.report.gtaDiscovery);
    if (preflight::IsBlocking(run.report.gtaDiscovery.status)) return run;

    run.report.exeFingerprint = preflight::FingerprintExecutable(run.report.gtaExePath);
    preflight::GtaExeVariant variant = preflight::GtaExeVariant::Unknown;
    run.report.exeVerification = preflight::EvaluateExeFingerprint(run.report.exeFingerprint, variant);
    if (progress) progress(preflight::PreflightStage::VerifyingExe, run.report.exeVerification);

    run.report.sampResult = preflight::InspectSampInstallation(run.report.gtaDir, launcherDir);
    run.report.sampInspection = preflight::EvaluateSampInspection(run.report.sampResult,
        run.report.bundledSampPath);
    if (progress) progress(preflight::PreflightStage::ScanningSamp, run.report.sampInspection);

    run.report.dependencies = preflight::InspectDependencies(run.report.gtaDir);
    run.report.depVerification = preflight::EvaluateDependencies(run.report.dependencies);
    if (progress) progress(preflight::PreflightStage::CheckingDeps, run.report.depVerification);

    run.report.bundledRuntime = preflight::EvaluateBundledRuntime(launcherDir, run.report.gtaDir,
        run.report.bundledSampPath, run.report.skrClientPath, run.report.cefDir);
    if (progress) progress(preflight::PreflightStage::PreparingLauncher, run.report.bundledRuntime);
    if (progress) progress(preflight::PreflightStage::Complete, {});
    return run;
}

std::string CandidateJson(const preflight::GtaCandidate& candidate, size_t index,
                          const std::wstring& selectedPath) {
    const auto fingerprint = preflight::FingerprintExecutable(candidate.exePath);
    preflight::GtaExeVariant variant = preflight::GtaExeVariant::Unknown;
    const auto verdict = preflight::EvaluateExeFingerprint(fingerprint, variant);
    const bool approved = verdict.status == preflight::CheckStatus::Verified;
    std::ostringstream aliases;
    for (size_t i = 0; i < candidate.aliases.size(); ++i) {
        if (i) aliases << ',';
        aliases << Quote(candidate.aliases[i]);
    }
    std::ostringstream out;
    out << "{\"index\":" << index
        << ",\"name\":" << Quote(FolderName(candidate.directory))
        << ",\"path\":" << Quote(candidate.directory)
        << ",\"exePath\":" << Quote(candidate.exePath)
        << ",\"source\":" << Quote(candidate.source)
        << ",\"version\":" << Quote(preflight::VariantToString(fingerprint.variant))
        << ",\"status\":" << Quote(StatusKey(verdict.status))
        << ",\"summary\":" << Quote(verdict.summary)
        << ",\"sha256\":" << Quote(preflight::Sha256ToString(fingerprint.sha256.data()))
        << ",\"structuralMatch\":" << (fingerprint.structuralMatch ? "true" : "false")
        << ",\"exactFingerprintVerified\":" << (fingerprint.exactFingerprintVerified ? "true" : "false")
        << ",\"compatibilityApproved\":" << (fingerprint.compatibilityApproved ? "true" : "false")
        << ",\"aliases\":[" << aliases.str() << "]"
        << ",\"approved\":" << (approved ? "true" : "false")
        << ",\"selected\":" << (SamePath(candidate.exePath, selectedPath) ? "true" : "false") << "}";
    return out.str();
}

std::string ResultJson(const VerificationRun& run) {
    const auto& report = run.report;
    const auto& blocker = PrimaryBlocker(report);
    int selectedIndex = -1;
    std::ostringstream candidates;
    for (size_t i = 0; i < run.candidates.size(); ++i) {
        if (i) candidates << ',';
        if (SamePath(run.candidates[i].exePath, report.gtaExePath)) selectedIndex = static_cast<int>(i);
        candidates << CandidateJson(run.candidates[i], i, report.gtaExePath);
    }

    std::ostringstream asi;
    for (size_t i = 0; i < report.sampResult.asiLoaders.size(); ++i) {
        if (i) asi << ',';
        asi << Quote(report.sampResult.asiLoaders[i]);
    }
    std::ostringstream components;
    for (size_t i = 0; i < report.sampResult.components.size(); ++i) {
        if (i) components << ',';
        components << "{\"name\":" << Quote(report.sampResult.components[i].name)
                   << ",\"version\":" << Quote(report.sampResult.components[i].recognizedVersion) << "}";
    }
    std::ostringstream dependencies;
    for (size_t i = 0; i < report.dependencies.size(); ++i) {
        if (i) dependencies << ',';
        const auto& dep = report.dependencies[i];
        dependencies << "{\"name\":" << Quote(dep.name)
                     << ",\"present\":" << (dep.present ? "true" : "false")
                     << ",\"detail\":" << Quote(dep.detail)
                     << ",\"foundPath\":" << Quote(dep.foundPath) << "}";
    }

    std::ostringstream out;
    out << "{\"v\":1,\"type\":\"preflight.result\",\"payload\":{"
        << "\"approved\":" << (NativeApproved(report) ? "true" : "false")
        << ",\"selectedIndex\":" << selectedIndex
        << ",\"selectedPath\":" << Quote(report.gtaDir)
        << ",\"blocker\":" << CheckJson(blocker)
        << ",\"checks\":{\"discovery\":" << CheckJson(report.gtaDiscovery)
        << ",\"executable\":" << CheckJson(report.exeVerification)
        << ",\"samp\":" << CheckJson(report.sampInspection)
        << ",\"dependencies\":" << CheckJson(report.depVerification)
        << ",\"runtime\":" << CheckJson(report.bundledRuntime) << "}"
        << ",\"candidates\":[" << candidates.str() << "]"
        << ",\"asiLoaders\":[" << asi.str() << "]"
        << ",\"sampComponents\":[" << components.str() << "]"
        << ",\"dependencies\":[" << dependencies.str() << "]"
        << ",\"executableIdentity\":{\"variant\":" << Quote(preflight::VariantToString(report.exeFingerprint.variant))
        << ",\"sha256\":" << Quote(preflight::Sha256ToString(report.exeFingerprint.sha256.data()))
        << ",\"machine\":" << report.exeFingerprint.peMachine
        << ",\"timestamp\":" << report.exeFingerprint.peTimestamp
        << ",\"imageSize\":" << report.exeFingerprint.peImageSize
        << ",\"fileSize\":" << report.exeFingerprint.fileSize
        << ",\"sectionCount\":" << report.exeFingerprint.sectionCount
        << ",\"structuralMatch\":" << (report.exeFingerprint.structuralMatch ? "true" : "false")
        << ",\"exactFingerprintVerified\":" << (report.exeFingerprint.exactFingerprintVerified ? "true" : "false")
        << ",\"compatibilityApproved\":" << (report.exeFingerprint.compatibilityApproved ? "true" : "false") << "}"
        << ",\"bundledSampPath\":" << Quote(report.bundledSampPath)
        << ",\"skrClientPath\":" << Quote(report.skrClientPath)
        << "}}";
    return out.str();
}

std::string ProgressJson(preflight::PreflightStage stage, const preflight::CheckResult& result) {
    std::ostringstream out;
    out << "{\"v\":1,\"type\":\"preflight.progress\",\"payload\":{"
        << "\"stage\":" << Quote(StageKey(stage))
        << ",\"index\":" << StageIndex(stage)
        << ",\"total\":" << preflight::kPreflightStageCount
        << ",\"label\":" << Quote(Utf8(preflight::StageToLabel(stage)))
        << ",\"status\":" << Quote(StatusKey(result.status))
        << ",\"summary\":" << Quote(result.summary) << "}}";
    return out.str();
}

int Scaled(int value, UINT dpi) { return MulDiv(value, static_cast<int>(dpi), 96); }

struct Window : std::enable_shared_from_this<Window> {
    HWND hwnd{};
    std::wstring launcherDir;
    std::wstring preferredPath;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> web;
    std::future<VerificationRun> verification;
    std::vector<preflight::GtaCandidate> candidates;
    preflight::PreflightReport report;
    bool reportReady = false;
    bool done = false;
    bool closeRequested = false;
    bool webViewAvailable = true;
    uint64_t activeRequestId = 0;
    std::wstring activeRequestPath;
    PreflightWindowResult result;
    std::mutex outboundMutex;
    std::deque<std::string> outbound;

    void Queue(std::string json) {
        std::lock_guard<std::mutex> lock(outboundMutex);
        outbound.push_back(std::move(json));
    }

    void QueueError(const char* message) {
        Queue(std::string("{\"v\":1,\"type\":\"preflight.host.error\",\"payload\":{\"message\":") +
              Quote(message) + "}}");
    }

    void PumpOutbound() {
        if (!web || done) return;
        std::deque<std::string> pending;
        {
            std::lock_guard<std::mutex> lock(outboundMutex);
            pending.swap(outbound);
        }
        for (const auto& json : pending) web->PostWebMessageAsJson(Wide(json).c_str());
    }

    void Resize(bool expanded) {
        if (!hwnd) return;
        const UINT dpi = GetDpiForWindow(hwnd);
        const int width = Scaled(expanded ? kExpandedWidth : kCompactWidth, dpi);
        const int height = Scaled(expanded ? kExpandedHeight : kCompactHeight, dpi);
        RECT current{};
        GetWindowRect(hwnd, &current);
        const int centerX = (current.left + current.right) / 2;
        const int centerY = (current.top + current.bottom) / 2;
        SetWindowPos(hwnd, nullptr, centerX - width / 2, centerY - height / 2, width, height,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void StartVerification(const std::wstring& path) {
        if (verification.valid()) return;
        const uint64_t requestId = ++activeRequestId;
        activeRequestPath = path;
        reportReady = false;
        Queue("{\"v\":1,\"type\":\"preflight.verification.started\",\"payload\":{}}");
        const auto weak = weak_from_this();
        const auto launcher = launcherDir;
        verification = std::async(std::launch::async, [weak, launcher, path, requestId] {
            return RunVerification(launcher, path, requestId, [weak, requestId](preflight::PreflightStage stage,
                                                          const preflight::CheckResult& check) {
                if (const auto keep = weak.lock(); keep && keep->activeRequestId == requestId)
                    keep->Queue(ProgressJson(stage, check));
            });
        });
    }

    void FinishVerification() {
        if (!verification.valid() ||
            verification.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        try {
            auto completed = verification.get();
            if (!preflight::IsCurrentVerification(activeRequestId, completed.requestId,
                    activeRequestPath, completed.requestedPath)) return;
            report = completed.report;
            candidates = completed.candidates;
            reportReady = true;
            preflight::WriteDiagnosticLog(report, report.gtaDir, launcherDir);
            Queue(ResultJson(completed));
        } catch (...) {
            reportReady = false;
            VerificationRun failed;
            failed.report.gtaDiscovery.status = preflight::CheckStatus::ActionRequired;
            failed.report.gtaDiscovery.code = "PREFLIGHT_NATIVE_ERROR";
            failed.report.gtaDiscovery.summary = "Native verification could not be completed";
            failed.report.gtaDiscovery.detail = "The native verifier stopped unexpectedly before producing a complete report.";
            failed.report.gtaDiscovery.remediation = "Retry verification. If the problem continues, open the diagnostic log and restart SKR Enhanced.";
            QueueError("Native verification failed unexpectedly.");
            Queue(ResultJson(failed));
        }
        if (closeRequested) done = true;
    }

    void RequestClose() {
        ++activeRequestId;
        activeRequestPath.clear();
        closeRequested = true;
        if (hwnd) ShowWindow(hwnd, SW_HIDE);
        if (!verification.valid()) done = true;
    }

    void Browse() {
        IFileOpenDialog* dialog = nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&dialog))) || !dialog) {
            QueueError("Windows could not open the folder picker.");
            return;
        }
        DWORD options = 0;
        if (SUCCEEDED(dialog->GetOptions(&options)))
            dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        dialog->SetTitle(L"Choose GTA San Andreas installation");
        if (SUCCEEDED(dialog->Show(hwnd))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item)) && item) {
                PWSTR raw = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) && raw) {
                    const auto exe = preflight::ResolveGtaExePath(raw);
                    CoTaskMemFree(raw);
                    if (exe.empty()) QueueError("That folder does not contain gta_sa.exe.");
                    else { Resize(false); StartVerification(exe); }
                }
                item->Release();
            }
        }
        dialog->Release();
    }

    void OpenLog() {
        wchar_t appData[MAX_PATH]{};
        if (SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appData) != S_OK) {
            QueueError("The diagnostic log path is unavailable.");
            return;
        }
        const std::wstring path = std::wstring(appData) + L"\\SKR\\preflight.log";
        const auto opened = reinterpret_cast<INT_PTR>(
            ShellExecuteW(hwnd, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        if (opened <= 32) QueueError("The diagnostic log could not be opened.");
    }

    void HandleMessage(const std::string& json) {
        if (json.size() > 16384 || done) return;
        std::map<std::string, lobby::detail::JsonScalar> fields;
        if (!lobby::detail::ParseFlatJson(json, fields)) return;
        const auto version = fields.find("v");
        const auto type = fields.find("type");
        if (version == fields.end() || version->second.quoted || version->second.text != "1" ||
            type == fields.end() || !type->second.quoted || type->second.text.size() > 64) return;

        const auto& action = type->second.text;
        if (action == "preflight.ready") { StartVerification(preferredPath); return; }
        if (action == "preflight.retry") {
            if (!verification.valid()) StartVerification(report.gtaExePath.empty() ? preferredPath : report.gtaExePath);
            return;
        }
        if (action == "preflight.installation.select") {
            const auto indexField = fields.find("index");
            if (indexField == fields.end() || indexField->second.quoted) return;
            char* end = nullptr;
            const unsigned long index = std::strtoul(indexField->second.text.c_str(), &end, 10);
            if (!end || *end || index >= candidates.size() || verification.valid()) return;
            Resize(false);
            StartVerification(candidates[index].exePath);
            return;
        }
        if (action == "preflight.installation.browse") { if (!verification.valid()) Browse(); return; }
        if (action == "preflight.diagnostics.open-log") { OpenLog(); return; }
        if (action == "preflight.continue") {
            if (!reportReady || !NativeApproved(report) || report.gtaExePath.empty()) {
                QueueError("Native approval is required before opening the lobby.");
                return;
            }
            result.approved = true;
            result.report = report;
            done = true;
            return;
        }
        if (action == "preflight.window.minimize") { ShowWindow(hwnd, SW_MINIMIZE); return; }
        if (action == "preflight.window.close") { RequestClose(); return; }
        if (action == "preflight.window.drag") {
            ReleaseCapture();
            SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            return;
        }
        if (action == "preflight.window.resize") {
            const auto expanded = fields.find("expanded");
            if (expanded != fields.end() && !expanded->second.quoted &&
                (expanded->second.text == "true" || expanded->second.text == "false"))
                Resize(expanded->second.text == "true");
        }
    }

    void Tick() {
        FinishVerification();
        PumpOutbound();
    }
};

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        state = static_cast<Window*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (state) {
        if (message == WM_CLOSE) { state->RequestClose(); return 0; }
        if (message == WM_TIMER && wParam == kTimerId) { state->Tick(); return 0; }
        if (message == WM_SIZE && state->controller) {
            RECT bounds{};
            GetClientRect(hwnd, &bounds);
            state->controller->put_Bounds(bounds);
            return 0;
        }
        if (message == WM_DPICHANGED) {
            const auto* suggested = reinterpret_cast<RECT*>(lParam);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                suggested->right - suggested->left, suggested->bottom - suggested->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace

PreflightWindowResult ShowPreflightExperience(HINSTANCE instance,
                                               const std::wstring& preflightDir,
                                               const std::wstring& launcherDir,
                                               const std::wstring& preferredGtaPath) {
    PreflightWindowResult unavailable;
    unavailable.webViewAvailable = false;
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(com)) return unavailable;

    auto state = std::make_shared<Window>();
    state->launcherDir = launcherDir;
    state->preferredPath = preferredGtaPath;

    const HINSTANCE iconModule = instance ? instance : GetModuleHandleW(nullptr);
    const HICON iconBig = static_cast<HICON>(LoadImageW(iconModule, MAKEINTRESOURCEW(kAppIconResourceId),
        IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_SHARED));
    const HICON iconSmall = static_cast<HICON>(LoadImageW(iconModule, MAKEINTRESOURCEW(kAppIconResourceId),
        IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));

    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kWindowClass;
    windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    windowClass.hbrBackground = CreateSolidBrush(RGB(32, 33, 32));
    windowClass.hIcon = iconBig;
    RegisterClassW(&windowClass);

    const UINT dpi = GetDpiForSystem();
    const int width = Scaled(kCompactWidth, dpi);
    const int height = Scaled(kCompactHeight, dpi);
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int x = work.left + ((work.right - work.left) - width) / 2;
    const int y = work.top + ((work.bottom - work.top) - height) / 2;
    state->hwnd = CreateWindowExW(0, kWindowClass, L"SKR Enhanced", WS_POPUP,
        x, y, width, height, nullptr, nullptr, instance, state.get());
    if (!state->hwnd) {
        CoUninitialize();
        return unavailable;
    }
    if (iconBig) SendMessageW(state->hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(iconBig));
    if (iconSmall) SendMessageW(state->hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(iconSmall));
    ShowWindow(state->hwnd, SW_SHOW);
    UpdateWindow(state->hwnd);
    SetTimer(state->hwnd, kTimerId, 50, nullptr);

    PWSTR local = nullptr;
    std::wstring profile;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) {
        profile = std::wstring(local) + L"\\SKR\\WebView2";
        CoTaskMemFree(local);
    }

    const auto failWebView = [state] { state->webViewAvailable = false; state->done = true; };
    const HRESULT environmentResult = profile.empty() ? E_FAIL :
        CreateCoreWebView2EnvironmentWithOptions(nullptr, profile.c_str(), nullptr,
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [state, preflightDir, failWebView](HRESULT error, ICoreWebView2Environment* environment) -> HRESULT {
                if (FAILED(error) || !environment) { failWebView(); return S_OK; }
                const HRESULT creating = environment->CreateCoreWebView2Controller(state->hwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                    [state, preflightDir, failWebView](HRESULT error, ICoreWebView2Controller* controller) -> HRESULT {
                        if (FAILED(error) || !controller) { failWebView(); return S_OK; }
                        state->controller = controller;
                        controller->get_CoreWebView2(&state->web);
                        if (!state->web) { failWebView(); return S_OK; }

                        ComPtr<ICoreWebView2Controller2> controller2;
                        if (SUCCEEDED(controller->QueryInterface(IID_PPV_ARGS(&controller2))) && controller2) {
                            COREWEBVIEW2_COLOR color{255, 32, 33, 32};
                            controller2->put_DefaultBackgroundColor(color);
                        }
                        ComPtr<ICoreWebView2Settings> settings;
                        if (SUCCEEDED(state->web->get_Settings(&settings)) && settings) {
                            settings->put_AreDefaultContextMenusEnabled(FALSE);
                            settings->put_AreDevToolsEnabled(FALSE);
                            settings->put_IsStatusBarEnabled(FALSE);
                        }
                        ComPtr<ICoreWebView2_3> mapping;
                        if (FAILED(state->web.As(&mapping)) || !mapping ||
                            FAILED(mapping->SetVirtualHostNameToFolderMapping(L"skr-preflight.invalid",
                                preflightDir.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS))) {
                            failWebView(); return S_OK;
                        }

                        EventRegistrationToken token{};
                        state->web->add_NavigationStarting(
                            Callback<ICoreWebView2NavigationStartingEventHandler>(
                            [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                                LPWSTR uri = nullptr;
                                args->get_Uri(&uri);
                                if (!uri || wcscmp(uri, kPage) != 0) args->put_Cancel(TRUE);
                                CoTaskMemFree(uri);
                                return S_OK;
                            }).Get(), &token);
                        state->web->add_NewWindowRequested(
                            Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                            [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                                args->put_Handled(TRUE);
                                return S_OK;
                            }).Get(), &token);
                        state->web->add_WebMessageReceived(
                            Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                            [weak = std::weak_ptr<Window>(state)](ICoreWebView2*,
                                ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                const auto keep = weak.lock();
                                if (!keep || keep->done) return S_OK;
                                LPWSTR source = nullptr;
                                LPWSTR json = nullptr;
                                args->get_Source(&source);
                                if (source && wcscmp(source, kPage) == 0 &&
                                    SUCCEEDED(args->get_WebMessageAsJson(&json)) && json)
                                    keep->HandleMessage(Utf8(json));
                                CoTaskMemFree(source);
                                CoTaskMemFree(json);
                                return S_OK;
                            }).Get(), &token);
                        state->web->add_NavigationCompleted(
                            Callback<ICoreWebView2NavigationCompletedEventHandler>(
                            [weak = std::weak_ptr<Window>(state), failWebView](ICoreWebView2*,
                                ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                                BOOL success = FALSE;
                                args->get_IsSuccess(&success);
                                if (!success && weak.lock()) failWebView();
                                return S_OK;
                            }).Get(), &token);

                        RECT bounds{};
                        GetClientRect(state->hwnd, &bounds);
                        controller->put_Bounds(bounds);
                        if (FAILED(state->web->Navigate(kPage))) failWebView();
                        return S_OK;
                    }).Get());
                if (FAILED(creating)) failWebView();
                return S_OK;
            }).Get());
    if (FAILED(environmentResult)) failWebView();

    MSG message{};
    while (!state->done) {
        const int status = GetMessageW(&message, nullptr, 0, 0);
        if (status <= 0) break;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    state->done = true;
    KillTimer(state->hwnd, kTimerId);
    if (state->controller) state->controller->Close();
    state->web.Reset();
    state->controller.Reset();
    DestroyWindow(state->hwnd);
    state->result.webViewAvailable = state->webViewAvailable;
    auto result = state->result;
    state.reset();
    CoUninitialize();
    return result;
}

} // namespace skr::launcher
