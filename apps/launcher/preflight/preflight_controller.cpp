#include "preflight_controller.h"
#include "game_installation_detector.h"
#include "exe_fingerprint.h"
#include "samp_inspector.h"
#include "dependency_inspector.h"
#include <shlobj.h>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <commctrl.h>
#include <windows.h>
#include <shellapi.h>
#include <objidl.h>
#include <gdiplus.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdiplus.lib")

namespace skr::preflight {

// ── Preflight execution ─────────────────────────────────────────────

PreflightReport RunPreflight(const std::wstring& launcherDir,
                              PreflightProgressCallback onProgress) {
    PreflightReport report;

    auto progress = [&](PreflightStage stage, const CheckResult& cr) {
        if (onProgress) onProgress(stage, cr);
    };

    // Stage 1: Locate GTA San Andreas
    auto candidates = DetectGtaInstallations(launcherDir);
    report.gtaDiscovery = EvaluateGtaDiscovery(candidates,
                                                report.gtaExePath,
                                                report.gtaDir);
    progress(PreflightStage::LocatingGta, report.gtaDiscovery);

    if (IsBlocking(report.gtaDiscovery.status)) {
        return report;
    }

    // Stage 2: Verify executable
    report.exeFingerprint = FingerprintExecutable(report.gtaExePath);
    GtaExeVariant variant = GtaExeVariant::Unknown;
    report.exeVerification = EvaluateExeFingerprint(report.exeFingerprint, variant);
    progress(PreflightStage::VerifyingExe, report.exeVerification);

    // Stage 3: Scan SA-MP components
    report.sampResult = InspectSampInstallation(report.gtaDir, launcherDir);
    report.sampInspection = EvaluateSampInspection(report.sampResult,
                                                    report.bundledSampPath);
    progress(PreflightStage::ScanningSamp, report.sampInspection);

    // Stage 4: Check runtime dependencies
    report.dependencies = InspectDependencies(report.gtaDir);
    report.depVerification = EvaluateDependencies(report.dependencies);
    progress(PreflightStage::CheckingDeps, report.depVerification);

    // Stage 5: Verify bundled runtime
    report.bundledRuntime = EvaluateBundledRuntime(
        launcherDir, report.gtaDir,
        report.bundledSampPath, report.skrClientPath, report.cefDir);
    progress(PreflightStage::PreparingLauncher, report.bundledRuntime);

    progress(PreflightStage::Complete, {});
    return report;
}

// ── Diagnostic log ──────────────────────────────────────────────────

static std::wstring GetLogPath() {
    wchar_t appData[MAX_PATH]{};
    if (SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appData) != S_OK)
        return {};
    std::wstring dir = std::wstring(appData) + L"\\SKR";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\preflight.log";
}

static std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                                   nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string s(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                        s.data(), len, nullptr, nullptr);
    return s;
}

bool WriteDiagnosticLog(const PreflightReport& report,
                         const std::wstring& gtaDir,
                         const std::wstring& launcherDir) {
    std::wstring logPath = GetLogPath();
    if (logPath.empty()) return false;

    std::ofstream log(logPath, std::ios::trunc);
    if (!log.is_open()) return false;

    time_t now = time(nullptr);
    char timeStr[64]{};
    struct tm tmBuf{};
    localtime_s(&tmBuf, &now);
    strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &tmBuf);

    log << "SKR Enhanced — Preflight Diagnostic Log\n";
    log << "Generated: " << timeStr << "\n";
    log << "Launcher:  " << WideToUtf8(launcherDir) << "\n";
    log << "GTA Dir:   " << WideToUtf8(gtaDir) << "\n";
    log << "GTA Exe:   " << WideToUtf8(report.gtaExePath) << "\n\n";

    auto writeCheck = [&](const char* label, const CheckResult& cr) {
        log << "── " << label << " ──\n";
        log << "  Status:      " << StatusToString(cr.status) << "\n";
        log << "  Code:        " << cr.code << "\n";
        log << "  Summary:     " << cr.summary << "\n";
        if (!cr.detail.empty())
            log << "  Detail:      " << cr.detail << "\n";
        if (!cr.remediation.empty())
            log << "  Remediation: " << cr.remediation << "\n";
        log << "\n";
    };

    writeCheck("GTA Discovery", report.gtaDiscovery);
    writeCheck("Executable Verification", report.exeVerification);

    if (report.exeFingerprint.identified) {
        log << "  Variant:     " << VariantToString(report.exeFingerprint.variant) << "\n";
        log << "  SHA-256:     " << Sha256ToString(report.exeFingerprint.sha256.data()) << "\n";
        char buf[32];
        snprintf(buf, sizeof(buf), "0x%08X", report.exeFingerprint.peTimestamp);
        log << "  PE Timestamp:" << buf << "\n";
        snprintf(buf, sizeof(buf), "0x%08X", report.exeFingerprint.peImageSize);
        log << "  PE ImageSize:" << buf << "\n";
        log << "  File Size:   " << report.exeFingerprint.fileSize << "\n\n";
    }

    writeCheck("SA-MP Inspection", report.sampInspection);

    if (!report.sampResult.components.empty()) {
        log << "  Components found:\n";
        for (auto& comp : report.sampResult.components) {
            log << "    - " << comp.name << " (" << comp.fileSize << " bytes)";
            if (!comp.recognizedVersion.empty())
                log << " [" << comp.recognizedVersion << "]";
            log << " SHA-256: " << Sha256ToString(comp.sha256.data()) << "\n";
        }
        log << "\n";
    }
    if (!report.sampResult.asiLoaders.empty()) {
        log << "  ASI files: ";
        for (auto& a : report.sampResult.asiLoaders)
            log << WideToUtf8(a) << " ";
        log << "\n\n";
    }

    writeCheck("Dependencies", report.depVerification);
    for (auto& dep : report.dependencies) {
        log << "    " << dep.name << ": "
            << (dep.present ? "OK" : "MISSING");
        if (!dep.foundPath.empty())
            log << " (" << WideToUtf8(dep.foundPath) << ")";
        log << "\n";
    }
    log << "\n";

    writeCheck("Bundled Runtime", report.bundledRuntime);
    log << "  SKR.Client.dll: " << WideToUtf8(report.skrClientPath) << "\n";
    log << "  Bundled samp:   " << WideToUtf8(report.bundledSampPath) << "\n";
    log << "  CEF dir:        " << WideToUtf8(report.cefDir) << "\n\n";

    log << "── Overall ──\n";
    log << "  All mandatory passed: "
        << (report.AllMandatoryPassed() ? "YES" : "NO") << "\n";

    log.close();
    return true;
}

// ════════════════════════════════════════════════════════════════════
//  Custom-drawn modern dark-themed preflight UI (GDI+)
// ════════════════════════════════════════════════════════════════════

// ── Theme constants ─────────────────────────────────────────────────

namespace theme {
    // Window
    constexpr int kWindowW         = 560;
    constexpr int kWindowH         = 520;
    constexpr int kPad             = 24;

    // Colors — dark surface
    constexpr DWORD kBgSurface     = 0xFF1A1A2E;  // deep navy
    constexpr DWORD kBgCard        = 0xFF24243E;  // slightly lighter card
    constexpr DWORD kBgCardHover   = 0xFF2D2D4A;  // card hover
    constexpr DWORD kBorder        = 0xFF3A3A5C;  // subtle border
    constexpr DWORD kTextPrimary   = 0xFFE8E8F0;  // almost white
    constexpr DWORD kTextSecondary = 0xFF9090A8;  // muted
    constexpr DWORD kTextMuted     = 0xFF6B6B82;  // dimmed

    // Status colors
    constexpr DWORD kGreen         = 0xFF4ADE80;  // success
    constexpr DWORD kAmber         = 0xFFFBBF24;  // warning
    constexpr DWORD kRed           = 0xFFEF4444;  // error
    constexpr DWORD kGreenDim      = 0xFF1A3A2A;  // success bg tint
    constexpr DWORD kAmberDim      = 0xFF3A3520;  // warning bg tint
    constexpr DWORD kRedDim        = 0xFF3A1A1A;  // error bg tint

    // Accent
    constexpr DWORD kAccent        = 0xFF6366F1;  // indigo primary
    constexpr DWORD kAccentHover   = 0xFF818CF8;  // indigo hover
    constexpr DWORD kAccentPress   = 0xFF4F46E5;  // indigo active

    // Buttons
    constexpr DWORD kBtnSecondary  = 0xFF2D2D4A;
    constexpr DWORD kBtnSecHover   = 0xFF3A3A5C;
    constexpr DWORD kBtnText       = 0xFFE8E8F0;

    // Geometry
    constexpr int kCardRadius      = 10;
    constexpr int kBtnRadius       = 8;
    constexpr int kCardPadX        = 16;
    constexpr int kCardPadY        = 14;
    constexpr int kCardGap         = 8;
    constexpr int kIconSize        = 22;
    constexpr int kBtnH            = 40;
    constexpr int kBtnW            = 140;
}

static const wchar_t* kPreflightWndClass = L"SKRPreflightModern";

static const int IDC_BTN_CONTINUE = 1001;
static const int IDC_BTN_EXIT     = 1002;
static const int IDC_BTN_LOG      = 1003;

// ── Helper: ARGB to GDI+ Color ─────────────────────────────────────

static Gdiplus::Color GpColor(DWORD argb) {
    return Gdiplus::Color(
        (BYTE)((argb >> 24) & 0xFF),
        (BYTE)((argb >> 16) & 0xFF),
        (BYTE)((argb >> 8) & 0xFF),
        (BYTE)(argb & 0xFF));
}

static std::wstring Utf8ToWideStr(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), len);
    return out;
}

// ── Draw helpers ────────────────────────────────────────────────────

static void FillRoundRect(Gdiplus::Graphics& g, const Gdiplus::RectF& rc,
                           float radius, const Gdiplus::Color& fill) {
    Gdiplus::GraphicsPath path;
    float d = radius * 2.0f;
    path.AddArc(rc.X, rc.Y, d, d, 180, 90);
    path.AddArc(rc.X + rc.Width - d, rc.Y, d, d, 270, 90);
    path.AddArc(rc.X + rc.Width - d, rc.Y + rc.Height - d, d, d, 0, 90);
    path.AddArc(rc.X, rc.Y + rc.Height - d, d, d, 90, 90);
    path.CloseFigure();
    Gdiplus::SolidBrush br(fill);
    g.FillPath(&br, &path);
}

static void DrawRoundRectBorder(Gdiplus::Graphics& g, const Gdiplus::RectF& rc,
                                 float radius, const Gdiplus::Color& color, float width = 1.0f) {
    Gdiplus::GraphicsPath path;
    float d = radius * 2.0f;
    path.AddArc(rc.X, rc.Y, d, d, 180, 90);
    path.AddArc(rc.X + rc.Width - d, rc.Y, d, d, 270, 90);
    path.AddArc(rc.X + rc.Width - d, rc.Y + rc.Height - d, d, d, 0, 90);
    path.AddArc(rc.X, rc.Y + rc.Height - d, d, d, 90, 90);
    path.CloseFigure();
    Gdiplus::Pen pen(color, width);
    g.DrawPath(&pen, &path);
}

// Status indicator: filled circle with icon
static void DrawStatusDot(Gdiplus::Graphics& g, float cx, float cy, float r,
                           CheckStatus status) {
    DWORD dotColor, dotBg;
    const wchar_t* icon = L"";
    switch (status) {
        case CheckStatus::Verified:
            dotColor = theme::kGreen; dotBg = theme::kGreenDim; icon = L"\x2713"; break;
        case CheckStatus::Warning:
            dotColor = theme::kAmber; dotBg = theme::kAmberDim; icon = L"!"; break;
        case CheckStatus::ActionRequired:
        case CheckStatus::Unsupported:
            dotColor = theme::kRed; dotBg = theme::kRedDim; icon = L"\x2715"; break;
        default:
            dotColor = theme::kTextMuted; dotBg = theme::kBgCard; icon = L"\x2022"; break;
    }
    // Background circle
    Gdiplus::SolidBrush bgBr(GpColor(dotBg));
    g.FillEllipse(&bgBr, cx - r, cy - r, r * 2.0f, r * 2.0f);
    // Icon text centered
    Gdiplus::SolidBrush fgBr(GpColor(dotColor));
    Gdiplus::Font iconFont(L"Segoe UI", 9.0f, Gdiplus::FontStyleBold,
                           Gdiplus::UnitPoint);
    Gdiplus::StringFormat sf;
    sf.SetAlignment(Gdiplus::StringAlignmentCenter);
    sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    Gdiplus::RectF iconRect(cx - r, cy - r, r * 2.0f, r * 2.0f);
    g.DrawString(icon, -1, &iconFont, iconRect, &sf, &fgBr);
}

// ── Button hit-testing ──────────────────────────────────────────────

struct BtnState {
    RECT rc;
    int id;
    bool hovered;
    bool pressed;
    bool primary;  // accent style
    const wchar_t* label;
};

struct PreflightWindowData {
    const PreflightReport* report;
    bool userContinue;

    // GDI+ token
    ULONG_PTR gdiplusToken;

    // Button states
    BtnState buttons[3];
    int buttonCount;

    // Tracking
    bool trackingMouse;
};

static int HitTestButton(PreflightWindowData* d, int x, int y) {
    for (int i = 0; i < d->buttonCount; ++i) {
        POINT pt = {x, y};
        if (PtInRect(&d->buttons[i].rc, pt))
            return i;
    }
    return -1;
}

// ── Draw a styled button ────────────────────────────────────────────

static void DrawButton(Gdiplus::Graphics& g, const BtnState& btn) {
    Gdiplus::RectF rc((float)btn.rc.left, (float)btn.rc.top,
                      (float)(btn.rc.right - btn.rc.left),
                      (float)(btn.rc.bottom - btn.rc.top));

    DWORD bgColor;
    if (btn.primary) {
        bgColor = btn.pressed ? theme::kAccentPress
                : btn.hovered ? theme::kAccentHover
                : theme::kAccent;
    } else {
        bgColor = btn.pressed ? theme::kBorder
                : btn.hovered ? theme::kBtnSecHover
                : theme::kBtnSecondary;
    }

    FillRoundRect(g, rc, (float)theme::kBtnRadius, GpColor(bgColor));
    if (!btn.primary) {
        DrawRoundRectBorder(g, rc, (float)theme::kBtnRadius,
                            GpColor(theme::kBorder), 1.0f);
    }

    // Label
    Gdiplus::Font font(L"Segoe UI", 10.0f,
                       btn.primary ? Gdiplus::FontStyleBold : Gdiplus::FontStyleRegular,
                       Gdiplus::UnitPoint);
    Gdiplus::SolidBrush textBr(GpColor(theme::kBtnText));
    Gdiplus::StringFormat sf;
    sf.SetAlignment(Gdiplus::StringAlignmentCenter);
    sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    g.DrawString(btn.label, -1, &font, rc, &sf, &textBr);
}

// ── Check row rendering ─────────────────────────────────────────────

struct CheckRowInfo {
    const CheckResult* cr;
    const wchar_t* label;     // Short stage label
};

static float DrawCheckCard(Gdiplus::Graphics& g, float x, float y, float w,
                            const CheckRowInfo& row) {
    using namespace theme;

    // Determine the secondary text (remediation first, then detail)
    std::wstring secondary;
    if (!row.cr->remediation.empty())
        secondary = Utf8ToWideStr(row.cr->remediation);
    else if (!row.cr->detail.empty())
        secondary = Utf8ToWideStr(row.cr->detail);

    // Measure height
    float cardH = (float)kCardPadY * 2.0f + 20.0f;  // base: padding + summary line
    if (!secondary.empty()) cardH += 18.0f;           // +detail line

    // Card background
    Gdiplus::RectF cardRect(x, y, w, cardH);
    FillRoundRect(g, cardRect, (float)kCardRadius, GpColor(kBgCard));

    // Left accent stripe
    {
        DWORD stripe = kBorder;
        if (row.cr->status == CheckStatus::Verified) stripe = kGreen;
        else if (row.cr->status == CheckStatus::Warning) stripe = kAmber;
        else if (IsBlocking(row.cr->status)) stripe = kRed;

        Gdiplus::GraphicsPath stripePath;
        float sr = (float)kCardRadius;
        stripePath.AddArc(x, y, sr * 2, sr * 2, 180, 90);
        stripePath.AddLine(x + sr, y, x + 4.0f, y);
        stripePath.AddLine(x + 4.0f, y, x + 4.0f, y + cardH);
        stripePath.AddArc(x, y + cardH - sr * 2, sr * 2, sr * 2, 90, 90);
        stripePath.CloseFigure();
        Gdiplus::SolidBrush stripeBr(GpColor(stripe));
        g.FillPath(&stripeBr, &stripePath);
    }

    // Status dot
    float dotCx = x + (float)kCardPadX + 4.0f + (float)kIconSize / 2.0f;
    float dotCy = y + cardH / 2.0f;
    DrawStatusDot(g, dotCx, dotCy, (float)kIconSize / 2.0f, row.cr->status);

    // Text left edge
    float textX = dotCx + (float)kIconSize / 2.0f + 12.0f;
    float textW = w - (textX - x) - (float)kCardPadX;

    // Summary line
    Gdiplus::Font fontSummary(L"Segoe UI", 10.5f, Gdiplus::FontStyleBold,
                               Gdiplus::UnitPoint);
    Gdiplus::SolidBrush textPrimBr(GpColor(kTextPrimary));
    std::wstring summary = Utf8ToWideStr(row.cr->summary);
    Gdiplus::RectF summaryRect(textX, y + (float)kCardPadY - 1.0f, textW, 20.0f);
    Gdiplus::StringFormat sfLeft;
    sfLeft.SetAlignment(Gdiplus::StringAlignmentNear);
    sfLeft.SetLineAlignment(Gdiplus::StringAlignmentNear);
    sfLeft.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
    sfLeft.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
    g.DrawString(summary.c_str(), -1, &fontSummary, summaryRect, &sfLeft, &textPrimBr);

    // Detail/remediation line
    if (!secondary.empty()) {
        Gdiplus::Font fontDetail(L"Segoe UI", 9.0f, Gdiplus::FontStyleRegular,
                                  Gdiplus::UnitPoint);
        Gdiplus::SolidBrush textSecBr(GpColor(kTextSecondary));
        Gdiplus::RectF detailRect(textX, y + (float)kCardPadY + 20.0f, textW, 18.0f);
        g.DrawString(secondary.c_str(), -1, &fontDetail, detailRect, &sfLeft, &textSecBr);
    }

    return cardH;
}

// ── Window procedure ────────────────────────────────────────────────

static LRESULT CALLBACK PreflightWndProc(HWND hwnd, UINT msg,
                                          WPARAM wParam, LPARAM lParam) {
    auto* d = reinterpret_cast<PreflightWindowData*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        d = reinterpret_cast<PreflightWindowData*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(d));
        d->trackingMouse = false;
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;  // skip — we paint the whole surface

    case WM_PAINT: {
        if (!d || !d->report) break;

        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT clientRc;
        GetClientRect(hwnd, &clientRc);
        int cw = clientRc.right, ch = clientRc.bottom;

        // Double buffer
        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBmp = CreateCompatibleBitmap(hdc, cw, ch);
        HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, memBmp);

        {
            Gdiplus::Graphics g(memDC);
            g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
            g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

            // Background
            Gdiplus::SolidBrush bgBr(GpColor(theme::kBgSurface));
            g.FillRectangle(&bgBr, 0, 0, cw, ch);

            float pad = (float)theme::kPad;
            float curY = pad;

            // ── Title ───────────────────────────────────────
            {
                Gdiplus::Font titleFont(L"Segoe UI", 16.0f, Gdiplus::FontStyleBold,
                                         Gdiplus::UnitPoint);
                Gdiplus::SolidBrush titleBr(GpColor(theme::kTextPrimary));
                Gdiplus::PointF titlePt(pad, curY);
                g.DrawString(L"Pre-Launch Check", -1, &titleFont, titlePt, &titleBr);
                curY += 30.0f;
            }

            // ── Status subtitle ─────────────────────────────
            {
                bool allPassed = d->report->AllMandatoryPassed();
                DWORD subColor = allPassed ? theme::kGreen : theme::kRed;
                const wchar_t* subText = allPassed
                    ? L"All checks passed \x2014 ready to launch"
                    : L"Issues found \x2014 resolve before launching";

                Gdiplus::Font subFont(L"Segoe UI", 10.0f, Gdiplus::FontStyleRegular,
                                       Gdiplus::UnitPoint);
                Gdiplus::SolidBrush subBr(GpColor(subColor));
                Gdiplus::PointF subPt(pad, curY);
                g.DrawString(subText, -1, &subFont, subPt, &subBr);
                curY += 28.0f;
            }

            // ── Separator line ──────────────────────────────
            {
                Gdiplus::Pen sepPen(GpColor(theme::kBorder), 1.0f);
                g.DrawLine(&sepPen, pad, curY, (float)cw - pad, curY);
                curY += 16.0f;
            }

            // ── Check cards ─────────────────────────────────
            float cardW = (float)cw - pad * 2.0f;
            const auto& r = *d->report;

            CheckRowInfo rows[] = {
                { &r.gtaDiscovery,    L"GTA Installation" },
                { &r.exeVerification, L"Executable" },
                { &r.sampInspection,  L"SA-MP" },
                { &r.depVerification, L"Dependencies" },
                { &r.bundledRuntime,  L"SKR Runtime" },
            };

            for (auto& row : rows) {
                float h = DrawCheckCard(g, pad, curY, cardW, row);
                curY += h + (float)theme::kCardGap;
            }

            // ── Buttons ─────────────────────────────────────
            for (int i = 0; i < d->buttonCount; ++i) {
                DrawButton(g, d->buttons[i]);
            }
        }

        BitBlt(hdc, 0, 0, cw, ch, memDC, 0, 0, SRCCOPY);
        SelectObject(memDC, oldBmp);
        DeleteObject(memBmp);
        DeleteDC(memDC);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_MOUSEMOVE: {
        if (!d) break;
        if (!d->trackingMouse) {
            TRACKMOUSEEVENT tme{};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);
            d->trackingMouse = true;
        }
        int mx = LOWORD(lParam), my = HIWORD(lParam);
        bool needRepaint = false;
        for (int i = 0; i < d->buttonCount; ++i) {
            bool wasHovered = d->buttons[i].hovered;
            POINT pt = {mx, my};
            d->buttons[i].hovered = PtInRect(&d->buttons[i].rc, pt) != 0;
            if (d->buttons[i].hovered != wasHovered) needRepaint = true;
        }
        if (needRepaint) InvalidateRect(hwnd, nullptr, FALSE);

        // Set cursor
        int hit = HitTestButton(d, mx, my);
        SetCursor(LoadCursor(nullptr, hit >= 0 ? IDC_HAND : IDC_ARROW));
        return 0;
    }

    case WM_MOUSELEAVE: {
        if (!d) break;
        d->trackingMouse = false;
        bool needRepaint = false;
        for (int i = 0; i < d->buttonCount; ++i) {
            if (d->buttons[i].hovered) { d->buttons[i].hovered = false; needRepaint = true; }
        }
        if (needRepaint) InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        if (!d) break;
        int mx = LOWORD(lParam), my = HIWORD(lParam);
        int hit = HitTestButton(d, mx, my);
        if (hit >= 0) {
            d->buttons[hit].pressed = true;
            SetCapture(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        if (!d) break;
        ReleaseCapture();
        int mx = LOWORD(lParam), my = HIWORD(lParam);
        for (int i = 0; i < d->buttonCount; ++i) {
            if (d->buttons[i].pressed) {
                d->buttons[i].pressed = false;
                POINT pt = {mx, my};
                if (PtInRect(&d->buttons[i].rc, pt)) {
                    // Fire button action
                    int id = d->buttons[i].id;
                    if (id == IDC_BTN_CONTINUE) {
                        d->userContinue = true;
                        DestroyWindow(hwnd);
                    } else if (id == IDC_BTN_EXIT) {
                        d->userContinue = false;
                        DestroyWindow(hwnd);
                    } else if (id == IDC_BTN_LOG) {
                        std::wstring logPath = GetLogPath();
                        if (!logPath.empty())
                            ShellExecuteW(hwnd, L"open", logPath.c_str(),
                                          nullptr, nullptr, SW_SHOW);
                    }
                }
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;
    }

    case WM_SETCURSOR:
        // Handled in WM_MOUSEMOVE
        if (LOWORD(lParam) == HTCLIENT) return TRUE;
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    case WM_CLOSE:
        if (d) d->userContinue = false;
        DestroyWindow(hwnd);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ── Show window ─────────────────────────────────────────────────────

bool ShowPreflightWindow(HINSTANCE hInstance, const PreflightReport& report) {
    // Initialize GDI+
    Gdiplus::GdiplusStartupInput gdipInit;
    ULONG_PTR gdipToken = 0;
    Gdiplus::GdiplusStartup(&gdipToken, &gdipInit, nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = PreflightWndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;  // We paint everything ourselves
    wc.lpszClassName = kPreflightWndClass;
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    PreflightWindowData data{};
    data.report = &report;
    data.userContinue = false;
    data.gdiplusToken = gdipToken;

    // Set up buttons
    bool allPassed = report.AllMandatoryPassed();
    int btnY = theme::kWindowH - theme::kPad - theme::kBtnH - 8;
    int btnIdx = 0;

    // View Log — always present, bottom-left
    data.buttons[btnIdx] = {
        {theme::kPad, btnY, theme::kPad + 110, btnY + theme::kBtnH},
        IDC_BTN_LOG, false, false, false, L"View Log"
    };
    btnIdx++;

    if (allPassed) {
        // Continue — primary, right-most
        data.buttons[btnIdx] = {
            {theme::kWindowW - theme::kPad - theme::kBtnW,
             btnY,
             theme::kWindowW - theme::kPad,
             btnY + theme::kBtnH},
            IDC_BTN_CONTINUE, false, false, true, L"Continue"
        };
        btnIdx++;

        // Exit — secondary, left of Continue
        data.buttons[btnIdx] = {
            {theme::kWindowW - theme::kPad - theme::kBtnW - 10 - 110,
             btnY,
             theme::kWindowW - theme::kPad - theme::kBtnW - 10,
             btnY + theme::kBtnH},
            IDC_BTN_EXIT, false, false, false, L"Exit"
        };
        btnIdx++;
    } else {
        // Close — primary, right-most
        data.buttons[btnIdx] = {
            {theme::kWindowW - theme::kPad - theme::kBtnW,
             btnY,
             theme::kWindowW - theme::kPad,
             btnY + theme::kBtnH},
            IDC_BTN_EXIT, false, false, true, L"Close"
        };
        btnIdx++;
    }
    data.buttonCount = btnIdx;

    // Calculate actual window size from client area
    RECT adjRect = {0, 0, theme::kWindowW, theme::kWindowH};
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRectEx(&adjRect, style, FALSE, WS_EX_APPWINDOW);
    int winW = adjRect.right - adjRect.left;
    int winH = adjRect.bottom - adjRect.top;

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);

    HWND hwnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        kPreflightWndClass,
        L"SKR Enhanced",
        style,
        (screenW - winW) / 2, (screenH - winH) / 2,
        winW, winH,
        nullptr, nullptr, hInstance, &data);

    if (!hwnd) {
        Gdiplus::GdiplusShutdown(gdipToken);
        return false;
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    UnregisterClassW(kPreflightWndClass, hInstance);
    Gdiplus::GdiplusShutdown(gdipToken);
    return data.userContinue;
}

} // namespace skr::preflight
