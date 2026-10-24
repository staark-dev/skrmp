// SKR Native Runtime API — ImGui notification & confirm UI (C3.1). See native_api_ui.h.
//
// All rendering uses ImGui draw lists in the D3D9 EndScene pass (same pass as
// the phone and GPS panel). Text metrics come from the loaded ImGui fonts so
// card heights match actual wrapped content.
#include "native-api/native_api_ui.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace skr::native_api::ui {
namespace {

namespace api = protocol::api;

// ── Palette (SKR Enhanced identity) ────────────────────────────────────
constexpr ImU32 kPanel      = IM_COL32( 18,  30,  40, 238);
constexpr ImU32 kPanelEdge  = IM_COL32( 36,  52,  66, 180);
constexpr ImU32 kTextPri    = IM_COL32(244, 248, 250, 255);
constexpr ImU32 kTextSec    = IM_COL32(172, 186, 200, 255);
constexpr ImU32 kBackdrop   = IM_COL32(  5,   7,   5, 140);
constexpr ImU32 kInfo       = IM_COL32(110, 176, 214, 255);
constexpr ImU32 kSuccess    = IM_COL32(168, 238, 101, 255);
constexpr ImU32 kWarning    = IM_COL32(240, 196,  90, 255);
constexpr ImU32 kDanger     = IM_COL32(240, 138, 131, 255);
constexpr ImU32 kAccentFill = IM_COL32(168, 238, 101, 255);
constexpr ImU32 kOnAccent   = IM_COL32(  9,  23,  27, 255);
constexpr ImU32 kSecBtn     = IM_COL32( 23,  33,  43, 255);
constexpr ImU32 kSecLine    = IM_COL32( 52,  78,  92, 255);

// ── Layout constants (in scaled units, reference = 768p) ───────────────
constexpr float kCardBaseW      = 282.0f;   // ~282px at 768p
constexpr float kCardMaxW       = 340.0f;   // absolute pixel cap
constexpr float kMarginR        = 16.0f;
constexpr float kMarginB        = 16.0f;
constexpr float kCardPadX       = 14.0f;
constexpr float kCardPadY       = 10.0f;
constexpr float kAccentBarW     =  4.0f;
constexpr float kCardRound      =  8.0f;
constexpr float kCardGap        =  8.0f;
constexpr float kPhoneGap       = 14.0f;
constexpr float kTitleSize      = 15.0f;    // font size at s=1 (Inter SemiBold)
constexpr float kBodySize       = 15.0f;    // (Inter Regular)
constexpr float kTitleBodyGap   =  3.0f;
constexpr float kAnimEnterMs    = 220.0f;
constexpr float kAnimExitMs     = 200.0f;
constexpr float kConfirmArmMs   = 400.0f;

constexpr size_t kMaxQueued  = 8;
constexpr size_t kMaxVisible = 3;

// ── Toast queue ────────────────────────────────────────────────────────
struct Toast {
    api::NotifyKind kind;
    char text[api::kMaxNotifyText + 1];
    uint64_t durationMs;
    uint64_t shownAt;       // 0 = not yet visible
};
Toast  g_toasts[kMaxQueued];
size_t g_toastCount = 0;

// ── Confirm dialog ─────────────────────────────────────────────────────
struct Dialog {
    bool active = false;
    uint32_t callId = 0;
    char title[api::kMaxConfirmTitle + 1] = {};
    char message[api::kMaxConfirmMessage + 1] = {};
    uint64_t openedAt = 0;
    uint64_t deadline = 0;
};
Dialog g_dialog;

struct PendingAnswer { bool pending = false; uint32_t callId = 0; api::ReplyResult result = api::ReplyResult::Cancelled; };
PendingAnswer g_answer;
uint64_t g_eatCharsUntil = 0;

uint64_t g_now = 0;

// ── Helpers ────────────────────────────────────────────────────────────
ImU32 KindAccent(api::NotifyKind k) {
    switch (k) {
        case api::NotifyKind::Success: return kSuccess;
        case api::NotifyKind::Warning: return kWarning;
        case api::NotifyKind::Error:   return kDanger;
        default:                       return kInfo;
    }
}

const char* KindLabel(api::NotifyKind k) {
    switch (k) {
        case api::NotifyKind::Success: return "SUCCESS";
        case api::NotifyKind::Warning: return "WARNING";
        case api::NotifyKind::Error:   return "ERROR";
        default:                       return "INFO";
    }
}

ImU32 WithAlpha(ImU32 col, float a) {
    a = std::clamp(a, 0.0f, 1.0f);
    const ImU32 orig = (col >> IM_COL32_A_SHIFT) & 0xFF;
    const ImU32 na = static_cast<ImU32>(orig * a);
    return (col & ~(0xFFu << IM_COL32_A_SHIFT)) | (na << IM_COL32_A_SHIFT);
}

float EaseOut(float t) { return 1.0f - (1.0f - t) * (1.0f - t); }

// Wrap text to maxW using ImGui font metrics, return bounding height.
float WrapTextHeight(ImFont* f, float size, const char* text, float maxW) {
    if (!f || !text || !*text) return size;
    const ImVec2 sz = f->CalcTextSizeA(size, FLT_MAX, maxW, text);
    return sz.y;
}

// Pick the font face closest to the requested size (same logic as phone_renderer).
ImFont* Pick(ImFont* heading, ImFont* body, float /*size*/) {
    (void)heading;
    return body ? body : ImGui::GetFont();
}

// ── Toast drawing ──────────────────────────────────────────────────────
void DrawToasts(const DrawContext& ctx) {
    if (!g_toastCount) return;

    ImDrawList* dl = ctx.dl;
    const float W = ctx.displayW, H = ctx.displayH;
    const float s = (std::max)(0.6f, H / 768.0f);
    ImFont* font = ctx.fontBody ? ctx.fontBody : ImGui::GetFont();

    const float cardW = (std::min)(kCardBaseW * s, kCardMaxW);
    const float padX = kCardPadX * s, padY = kCardPadY * s;
    const float barW = kAccentBarW * s;
    const float round = kCardRound * s;
    const float gap = kCardGap * s;
    const float titleSz = kTitleSize * s;
    const float bodySz = kBodySize * s;
    const float tbGap = kTitleBodyGap * s;
    const float marginR = kMarginR * s;
    const float marginB = kMarginB * s;

    // Anchor: bottom-right, shifted left if the phone is open.
    float anchorR = W - marginR;
    if (ctx.phoneVisible && ctx.phoneLeft > 0.0f) {
        const float leftOfPhone = ctx.phoneLeft - kPhoneGap * s;
        if (leftOfPhone > cardW + marginR)
            anchorR = leftOfPhone;
    }
    const float cardX0 = anchorR - cardW;
    const float cardX1 = anchorR;

    // Safe-area bottom: stay above minimap (≈ bottom 22% for round radar).
    const float safeBottom = H - (std::max)(marginB, H * 0.02f);

    // Stack from bottom up.
    float curBottom = safeBottom;
    for (size_t i = 0; i < g_toastCount && i < kMaxVisible; ++i) {
        const Toast& t = g_toasts[i];
        if (!t.shownAt || g_now < t.shownAt) continue;
        const uint64_t age = g_now - t.shownAt;
        if (age >= t.durationMs) continue;

        // Animation alpha and slide offset.
        float alpha = 1.0f;
        float slideX = 0.0f;
        if (static_cast<float>(age) < kAnimEnterMs) {
            const float p = static_cast<float>(age) / kAnimEnterMs;
            alpha = EaseOut(p);
            slideX = (1.0f - EaseOut(p)) * (cardW * 0.3f);
        } else if (static_cast<float>(t.durationMs - age) < kAnimExitMs) {
            const float p = static_cast<float>(t.durationMs - age) / kAnimExitMs;
            alpha = p;
        }

        // Measure text to compute card height.
        const float textW = cardW - barW - padX * 2.0f;
        const char* label = KindLabel(t.kind);
        const float labelH = font->CalcTextSizeA(titleSz, FLT_MAX, 0.0f, label).y;
        const float bodyH = WrapTextHeight(font, bodySz, t.text, textW);
        const float cardH = padY + labelH + tbGap + bodyH + padY;

        // Position this card.
        const float y1 = curBottom;
        const float y0 = y1 - cardH;
        const float x0 = cardX0 + slideX;
        const float x1 = cardX1 + slideX;

        // Don't draw if off-screen.
        if (y0 < 0.0f) break;

        const ImU32 accent = KindAccent(t.kind);

        // Card background.
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1),
                          WithAlpha(kPanel, alpha), round);
        // Subtle border.
        dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1),
                    WithAlpha(kPanelEdge, alpha * 0.5f), round, 0, 1.0f);
        // Accent bar (left edge, clipped to card rounding).
        dl->AddRectFilled(ImVec2(x0, y0 + round), ImVec2(x0 + barW, y1 - round),
                          WithAlpha(accent, alpha));

        // Category label.
        const float textX = x0 + barW + padX;
        float textY = y0 + padY;
        dl->AddText(font, titleSz, ImVec2(textX, textY),
                    WithAlpha(accent, alpha), label);
        textY += labelH + tbGap;

        // Body text (wrapped).
        dl->AddText(font, bodySz, ImVec2(textX, textY),
                    WithAlpha(kTextPri, alpha), t.text, nullptr, textW);

        curBottom = y0 - gap;
    }
}

// ── Confirm dialog drawing ─────────────────────────────────────────────
void DrawConfirm(const DrawContext& ctx) {
    const Dialog& d = g_dialog;
    ImDrawList* dl = ctx.dl;
    const float W = ctx.displayW, H = ctx.displayH;
    const float s = (std::max)(0.6f, H / 768.0f);
    ImFont* fh = ctx.fontLarge ? ctx.fontLarge : (ctx.fontHeading ? ctx.fontHeading : ImGui::GetFont());
    ImFont* fb = ctx.fontBody ? ctx.fontBody : ImGui::GetFont();

    const float dialogW = (std::min)(460.0f * s, W - 32.0f);
    const float pad = 24.0f * s;
    const float round = 10.0f * s;
    const float titleSz = 20.0f * s;
    const float msgSz = 16.0f * s;
    const float innerW = dialogW - pad * 2.0f;

    const float titleH = WrapTextHeight(fh, titleSz, d.title, innerW);
    const float msgH = WrapTextHeight(fb, msgSz, d.message, innerW);
    const float chipH = 40.0f * s, chipGap = 10.0f * s;
    const float dialogH = pad + titleH + 14.0f * s + msgH + 20.0f * s + chipH + pad;

    const float x0 = (W - dialogW) * 0.5f, y0 = (H - dialogH) * 0.5f;
    const float x1 = x0 + dialogW, y1 = y0 + dialogH;

    // Backdrop.
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), kBackdrop);

    // Dialog card.
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), kPanel, round);
    dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), kPanelEdge, round, 0, 1.0f);

    // Accent stripe at top.
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y0 + 4.0f * s), kAccentFill,
                      round, ImDrawFlags_RoundCornersTop);

    const bool armed = g_now - d.openedAt >= static_cast<uint64_t>(kConfirmArmMs);
    const float armAlpha = armed ? 1.0f : 0.45f;

    // Title.
    float cy = y0 + pad;
    dl->AddText(fh, titleSz, ImVec2(x0 + pad, cy), kTextPri, d.title, nullptr, innerW);
    cy += titleH + 14.0f * s;

    // Message.
    dl->AddText(fb, msgSz, ImVec2(x0 + pad, cy), kTextSec, d.message, nullptr, innerW);

    // Buttons.
    const float btnY = y1 - pad - chipH;
    const float confirmW = 160.0f * s, cancelW = 140.0f * s;

    // Confirm button (primary).
    dl->AddRectFilled(ImVec2(x0 + pad, btnY), ImVec2(x0 + pad + confirmW, btnY + chipH),
                      WithAlpha(kAccentFill, armAlpha), 6.0f * s);
    {
        const char* lbl = "[Y] CONFIRM";
        const float btnSz = 15.0f * s;
        const ImVec2 sz = fb->CalcTextSizeA(btnSz, FLT_MAX, 0.0f, lbl);
        dl->AddText(fb, btnSz,
                    ImVec2(x0 + pad + (confirmW - sz.x) * 0.5f, btnY + (chipH - sz.y) * 0.5f),
                    WithAlpha(kOnAccent, armAlpha), lbl);
    }

    // Cancel button (secondary).
    const float cancelX = x0 + pad + confirmW + chipGap;
    dl->AddRectFilled(ImVec2(cancelX, btnY), ImVec2(cancelX + cancelW, btnY + chipH),
                      WithAlpha(kSecBtn, armAlpha), 6.0f * s);
    dl->AddRect(ImVec2(cancelX, btnY), ImVec2(cancelX + cancelW, btnY + chipH),
                WithAlpha(kSecLine, armAlpha), 6.0f * s, 0, 1.0f);
    {
        const char* lbl = "[N] CANCEL";
        const float btnSz = 15.0f * s;
        const ImVec2 sz = fb->CalcTextSizeA(btnSz, FLT_MAX, 0.0f, lbl);
        dl->AddText(fb, btnSz,
                    ImVec2(cancelX + (cancelW - sz.x) * 0.5f, btnY + (chipH - sz.y) * 0.5f),
                    WithAlpha(kTextPri, armAlpha), lbl);
    }

    // Countdown.
    const uint64_t left = d.deadline > g_now ? (d.deadline - g_now + 999) / 1000 : 0;
    char secs[16];
    snprintf(secs, sizeof(secs), "%llus", static_cast<unsigned long long>(left));
    const float secSz = 14.0f * s;
    const ImVec2 secW = fb->CalcTextSizeA(secSz, FLT_MAX, 0.0f, secs);
    dl->AddText(fb, secSz,
                ImVec2(x1 - pad - secW.x, btnY + (chipH - secW.y) * 0.5f),
                left <= 5 ? kWarning : kTextSec, secs);
}

void Resolve(api::ReplyResult result) {
    g_answer = {true, g_dialog.callId, result};
    g_dialog = {};
}

bool IsDialogKey(uintptr_t vk) {
    return vk == 'Y' || vk == 'N' || vk == VK_RETURN || vk == VK_ESCAPE;
}

} // namespace

void PushNotification(api::NotifyKind kind, uint8_t durationSec, const char* text) {
    if (!text) return;
    if (g_toastCount == kMaxQueued) {
        memmove(&g_toasts[0], &g_toasts[1], sizeof(Toast) * (kMaxQueued - 1));
        --g_toastCount;
    }
    Toast& t = g_toasts[g_toastCount++];
    t.kind = kind;
    snprintf(t.text, sizeof(t.text), "%s", text);
    t.durationMs = static_cast<uint64_t>(durationSec) * 1000;
    t.shownAt = 0;
}

bool ConfirmActive() { return g_dialog.active; }
uint32_t ConfirmCallId() { return g_dialog.callId; }

void OpenConfirm(uint32_t callId, const char* title, const char* message, uint8_t timeoutSec) {
    g_dialog = {};
    g_dialog.active = true;
    g_dialog.callId = callId;
    snprintf(g_dialog.title, sizeof(g_dialog.title), "%s", title);
    snprintf(g_dialog.message, sizeof(g_dialog.message), "%s", message);
    g_dialog.openedAt = GetTickCount64();
    g_dialog.deadline = g_dialog.openedAt + static_cast<uint64_t>(timeoutSec) * 1000;
}

void CloseConfirm() { g_dialog = {}; }

bool TakeConfirmAnswer(uint32_t& callId, api::ReplyResult& result) {
    if (!g_answer.pending) return false;
    callId = g_answer.callId;
    result = g_answer.result;
    g_answer = {};
    return true;
}

bool WantsRender() {
    return g_toastCount > 0 || g_dialog.active;
}

void Tick(uint64_t nowMs) {
    g_now = nowMs;
    if (g_dialog.active && nowMs >= g_dialog.deadline) Resolve(api::ReplyResult::Timeout);
    for (size_t i = 0; i < g_toastCount && i < kMaxVisible; ++i)
        if (!g_toasts[i].shownAt) g_toasts[i].shownAt = nowMs;
    size_t keep = 0;
    for (size_t i = 0; i < g_toastCount; ++i) {
        const Toast& t = g_toasts[i];
        if (t.shownAt && nowMs - t.shownAt >= t.durationMs) continue;
        if (keep != i) g_toasts[keep] = t;
        ++keep;
    }
    g_toastCount = keep;
}

void DrawImGui(const DrawContext& ctx) {
    if (!ctx.dl) return;
    if (!g_toastCount && !g_dialog.active) return;
    __try {
        if (g_toastCount) DrawToasts(ctx);
        if (g_dialog.active) DrawConfirm(ctx);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_toastCount = 0;
        if (g_dialog.active) Resolve(api::ReplyResult::Unsupported);
    }
}

static bool IsDialogChar(uintptr_t ch) {
    return ch == 'y' || ch == 'Y' || ch == 'n' || ch == 'N' || ch == '\r' || ch == 0x1B;
}

bool OnWndProc(unsigned int msg, uintptr_t wParam, intptr_t lParam) {
    if (!g_dialog.active)
        return msg == WM_CHAR && IsDialogChar(wParam) && GetTickCount64() < g_eatCharsUntil;
    switch (msg) {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            if (!IsDialogKey(wParam)) return false;
            const bool repeat = (static_cast<uint32_t>(lParam) & (1u << 30)) != 0;
            if (repeat || GetTickCount64() - g_dialog.openedAt < static_cast<uint64_t>(kConfirmArmMs)) return true;
            Resolve((wParam == 'Y' || wParam == VK_RETURN) ? api::ReplyResult::Confirmed
                                                           : api::ReplyResult::Cancelled);
            g_eatCharsUntil = GetTickCount64() + 250;
            return true;
        }
        case WM_KEYUP:
        case WM_SYSKEYUP:
            return IsDialogKey(wParam);
        case WM_CHAR:
            return IsDialogChar(wParam);
        default:
            return false;
    }
}

void Reset() {
    g_toastCount = 0;
    g_dialog = {};
    g_answer = {};
    g_eatCharsUntil = 0;
}

} // namespace skr::native_api::ui
