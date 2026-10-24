// ═══════════════════════════════════════════════════════════════════════
// SKR Native Pause Menu — Renderer
// Dear ImGui (Win32/DX9 backend) for layout/shapes + GTA SA CFont for
// all visible text.  Two-pass rendering:
//   Pass 1: ImGui draws backgrounds, rectangles, separators, buttons.
//   Pass 2: CFont text queue is flushed after ImGui::Render() so text
//           appears on top of the ImGui geometry.
//
// Typography mapping (Task #22 Additional Requirement):
//   FONT_GOTHIC    (1) — headings, menu title "SKR ENHANCED"
//   FONT_MENU      (2) — tab labels, button text, navigation
//   FONT_SUBTITLES (0) — body/descriptive text, info panels
//   FONT_PRICEDOWN (3) — version number, accents
//
// CFont uses GTA's 640×448 virtual coordinate space.
//   Screen X = virtualX * screenWidth  / 640.0f
//   Screen Y = virtualY * screenHeight / 448.0f
// We convert pixel positions from ImGui's screen-space layout into
// CFont's virtual space for PrintString calls.
//
// CFont::PrintString adds to an internal buffer; we call
// CFont::RenderFontBuffer (0x719840) once after all text is queued
// to flush the batch to the screen.
// ═══════════════════════════════════════════════════════════════════════

#include "native-pause/pause_renderer.h"
#include "native-pause/pause_enhanced.h"
#include "native-pause/pause_input.h"
#include "native-pause/pause_actions.h"
#include "native-pause/pause_controls.h"
#include "native-pause/pause_settings.h"
#include "native-pause/pause_map.h"
#include "game-state/game_state.h"
#include "game-state/radar_hud.h"
#include "gps/gps_flags.h"
#include "gps/gps_engine.h"
#include "gps/gps_panel.h"
#include "native-pause/map_route_draw.h"
#include "phone/phone_input.h"
#include "phone/phone_wallpaper.h"
#include "native-api/native_api_ui.h"

#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx9.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#include <cstdint>
#include <cfloat>
#include <algorithm>
#include <cmath>
#include <cstring>

// Forward-declare the ImGui Win32 handler (needed for mouse/keyboard input)
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace skr::native_pause::renderer {
namespace {

// ── Log ─────────────────────────────────────────────────────────────
static char g_logPath[MAX_PATH] = {};
static bool g_logPathOk = false;

static const char* GetLogPath() {
    if (g_logPathOk) return g_logPath;
    DWORD n = GetModuleFileNameA(nullptr, g_logPath, MAX_PATH);
    if (!n || n >= MAX_PATH) return nullptr;
    char* slash = strrchr(g_logPath, '\\');
    if (!slash) return nullptr;
    snprintf(slash + 1, MAX_PATH - (slash + 1 - g_logPath), "SKR\\native_pause.log");
    g_logPathOk = true;
    return g_logPath;
}

static void Log(const char* fmt, ...) {
    if (!skr::native_pause::enhanced::DebugLogEnabled()) return;
    const char* path = GetLogPath();
    if (!path) return;
    FILE* f = fopen(path, "a");
    if (!f) return;
    fprintf(f, "[%lu] RENDERER: ", GetTickCount());
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fprintf(f, "\n");
    fclose(f);
}

// ═══════════════════════════════════════════════════════════════════════
// GTA SA CFont Interface
// ═══════════════════════════════════════════════════════════════════════
// Provenance: plugin-sdk CFont.h / gta-reversed CFont.cpp
// GTA SA 1.0 US addresses — CFont is a static class, all methods __cdecl.
//
// CFont uses D3D9 directly (CSprite2d::Draw) for its sprite rendering,
// not RenderWare's 2D pipeline.  Font textures are loaded by GTA at
// startup and persist.  We save/restore D3D9 state around our calls.
//
// CFont::PrintString buffers glyphs internally.  We must call
// CFont::RenderFontBuffer() to flush the buffer and actually draw.

namespace cfont {

// Font style indices (CFont::SetFontStyle parameter)
constexpr uint8_t FONT_SUBTITLES  = 0;  // Clean sans — body text
constexpr uint8_t FONT_GOTHIC     = 1;  // Bold gothic — headings
constexpr uint8_t FONT_MENU       = 2;  // Menu font — navigation / tabs
constexpr uint8_t FONT_PRICEDOWN  = 3;  // Pricedown — GTA accent font

// CFont static method addresses (GTA SA 1.0 US)
constexpr uintptr_t kSetFontStyle          = 0x719490;  // void(uint8_t style)
constexpr uintptr_t kSetScale              = 0x719380;  // void(float w, float h)
constexpr uintptr_t kSetColor              = 0x719430;  // void(CRGBA color)
constexpr uintptr_t kSetJustify            = 0x719600;  // void(bool on)
constexpr uintptr_t kSetCentreSize         = 0x7194E0;  // void(float width)
constexpr uintptr_t kSetRightJustifyOn     = 0x7195C0;  // void()
constexpr uintptr_t kSetBackground         = 0x7195E0;  // void(bool on) — FIXED: was 0x7195B0
constexpr uintptr_t kSetProportional       = 0x7195B0;  // void(bool on)
constexpr uintptr_t kSetCentre             = 0x719610;  // void(bool on) — centre text
constexpr uintptr_t kSetWrapx              = 0x7194D0;  // void(float x)
constexpr uintptr_t kSetDropShadowPosition = 0x719570;  // void(int16_t pos)
constexpr uintptr_t kSetDropColor          = 0x719510;  // void(CRGBA color)
constexpr uintptr_t kSetEdge               = 0x719590;  // void(int16_t edge)
constexpr uintptr_t kPrintString           = 0x71A700;  // void(float x, float y, const char* str)
constexpr uintptr_t kRenderFontBuffer      = 0x719840;  // void() — ADDED: flushes text buffer

// CRGBA for CFont — packed 4 bytes
struct CRGBA {
    uint8_t r, g, b, a;
};

// Type aliases for CFont functions
using SetFontStyleFn     = void(__cdecl*)(uint8_t);
using SetScaleFn         = void(__cdecl*)(float, float);
using SetColorFn         = void(__cdecl*)(CRGBA);
using SetJustifyFn       = void(__cdecl*)(bool);
using SetCentreSizeFn    = void(__cdecl*)(float);
using SetBackgroundFn    = void(__cdecl*)(bool);
using SetProportionalFn  = void(__cdecl*)(bool);
using SetCentreFn        = void(__cdecl*)(bool);
using SetWrapxFn         = void(__cdecl*)(float);
using SetDropShadowFn    = void(__cdecl*)(int16_t);
using SetDropColorFn     = void(__cdecl*)(CRGBA);
using SetEdgeFn          = void(__cdecl*)(int16_t);
using PrintStringFn      = void(__cdecl*)(float, float, const char*);
using RenderFontBufferFn = void(__cdecl*)();

// Global availability flag
bool g_cfontAvailable = false;

bool VerifyCFont() {
    __try {
        auto setStyle = reinterpret_cast<SetFontStyleFn>(kSetFontStyle);
        setStyle(FONT_SUBTITLES);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Render a single text string via CFont.
// x, y are in CFont's 640×448 virtual coordinate space.
void RenderText(float x, float y, const char* text, uint8_t font,
                float scaleW, float scaleH, CRGBA color,
                bool centre = false, int16_t shadow = 1,
                float wrapX = 640.0f) {
    if (!g_cfontAvailable) return;

    __try {
        auto setStyle    = reinterpret_cast<SetFontStyleFn>(kSetFontStyle);
        auto setScale    = reinterpret_cast<SetScaleFn>(kSetScale);
        auto setColor    = reinterpret_cast<SetColorFn>(kSetColor);
        auto setJustify  = reinterpret_cast<SetJustifyFn>(kSetJustify);
        auto setBg       = reinterpret_cast<SetBackgroundFn>(kSetBackground);
        auto setProp     = reinterpret_cast<SetProportionalFn>(kSetProportional);
        auto setCentre   = reinterpret_cast<SetCentreFn>(kSetCentre);
        auto setWrapx    = reinterpret_cast<SetWrapxFn>(kSetWrapx);
        auto setShadow   = reinterpret_cast<SetDropShadowFn>(kSetDropShadowPosition);
        auto setShadowC  = reinterpret_cast<SetDropColorFn>(kSetDropColor);
        auto setEdge     = reinterpret_cast<SetEdgeFn>(kSetEdge);
        auto print       = reinterpret_cast<PrintStringFn>(kPrintString);

        setStyle(font);
        setScale(scaleW, scaleH);
        setColor(color);
        setBg(false);
        setProp(true);
        setCentre(centre);
        setJustify(!centre);
        setWrapx(wrapX);
        setShadow(shadow);
        setShadowC({0, 0, 0, 200});
        setEdge(0);
        print(x, y, text);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_cfontAvailable = false;
        Log("CFont::RenderText SEH exception — disabling CFont");
    }
}

// Flush CFont's internal text buffer so all queued PrintString calls render.
void FlushFontBuffer() {
    if (!g_cfontAvailable) return;

    __try {
        auto flush = reinterpret_cast<RenderFontBufferFn>(kRenderFontBuffer);
        flush();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_cfontAvailable = false;
        Log("CFont::RenderFontBuffer SEH exception — disabling CFont");
    }
}

} // namespace cfont

// ═══════════════════════════════════════════════════════════════════════
// CFont Text Queue
// ═══════════════════════════════════════════════════════════════════════
// During the ImGui layout pass (NewFrame → EndFrame) we cannot call
// CFont directly because ImGui owns the D3D9 state.  Instead we queue
// text entries and flush them AFTER ImGui::Render()/RenderDrawData().
//
// If CFont is unavailable, QueueText() falls back to ImGui's AddText()
// immediately (during the ImGui frame) so text still appears.

// ── ImGui fallback typography ───────────────────────────────────────
// CFont is disabled after SA-MP swaps in its proxy device (see Render()), so
// in multiplayer this path draws all menu text. It mirrors CFont semantics:
// scaleH maps to a pixel height relative to GTA's 448-line virtual screen,
// centre=true centres on x, shadow>0 adds a drop shadow.
constexpr float kFallbackPxPerScale = 11.0f;
// Milestone A.1: every queued text is enlarged once here (both the CFont
// queue and the ImGui fallback), so call sites keep their relative sizes.
constexpr float kTextBoost = 1.35f;
constexpr float kMinTextPx = 13.0f;   // floor for low resolutions
ImFont* g_fontHeading = nullptr;   // FONT_GOTHIC / FONT_MENU / FONT_PRICEDOWN
ImFont* g_fontBody    = nullptr;   // FONT_SUBTITLES
bool   g_textClipActive = false;   // text outside the content view is skipped
ImVec2 g_textClipMin;
ImVec2 g_textClipMax;
float  g_textAlpha = 1.0f;         // menu fade (foreground list ignores style alpha)

float TextPixelHeight(float scaleH) {
    float sh = ImGui::GetIO().DisplaySize.y;
    if (sh <= 0.0f) sh = 600.0f;
    return fmaxf(kMinTextPx, scaleH * kTextBoost * kFallbackPxPerScale * sh / 448.0f);
}

ImFont* FallbackFont(uint8_t font) {
    ImFont* f = (font == cfont::FONT_SUBTITLES) ? g_fontBody : g_fontHeading;
    return f ? f : ImGui::GetFont();
}

bool TextOutsideClip(float y, float px) {
    if (!g_textClipActive) return false;
    return y < g_textClipMin.y - 1.0f || y + px * 0.8f > g_textClipMax.y + 1.0f;
}

void DrawFallbackText(float x, float y, const char* text, uint8_t font,
                      float scaleH, cfont::CRGBA color, bool centre, int16_t shadow) {
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (!dl || !text || !*text) return;
    ImFont* f = FallbackFont(font);
    const float px = TextPixelHeight(scaleH);
    if (centre) x -= f->CalcTextSizeA(px, FLT_MAX, 0.0f, text).x * 0.5f;
    x = floorf(x + 0.5f);
    y = floorf(y + 0.5f);
    const float a = std::clamp(g_textAlpha, 0.0f, 1.0f);
    if (g_textClipActive) dl->PushClipRect(g_textClipMin, g_textClipMax, true);
    (void)shadow;   // 22.5.1: flat text, no drop shadows anywhere in the ESC UI
    dl->AddText(f, px, ImVec2(x, y),
                IM_COL32(color.r, color.g, color.b, static_cast<int>(color.a * a)), text);
    if (g_textClipActive) dl->PopClipRect();
}

struct TextEntry {
    float x, y;          // CFont virtual coords (640×448 space)
    char  text[256];
    uint8_t font;
    float scaleW, scaleH;
    cfont::CRGBA color;
    bool  centre;
    int16_t shadow;
    float wrapX;
};

constexpr int kMaxTextEntries = 256;
TextEntry g_textQueue[kMaxTextEntries];
int       g_textQueueCount = 0;

void ResetTextQueue() {
    g_textQueueCount = 0;
}

// Convert screen-space pixel coordinates to CFont's 640×448 virtual space.
float ScreenToVirtualX(float screenX, float screenW) {
    return screenX * 640.0f / screenW;
}
float ScreenToVirtualY(float screenY, float screenH) {
    return screenY * 448.0f / screenH;
}

// Queue text for CFont rendering (post-ImGui), or fall back to ImGui AddText.
// screenX/screenY are in actual pixel coordinates (ImGui screen space).
void QueueText(float screenX, float screenY, const char* text,
               uint8_t font, float scaleW, float scaleH,
               cfont::CRGBA color, bool centre = false,
               int16_t shadow = 1, float wrapVirtualX = 640.0f) {

    if (TextOutsideClip(screenY, TextPixelHeight(scaleH))) return;

    float sw = ImGui::GetIO().DisplaySize.x;
    float sh = ImGui::GetIO().DisplaySize.y;

    if (cfont::g_cfontAvailable && g_textQueueCount < kMaxTextEntries) {
        TextEntry& e = g_textQueue[g_textQueueCount++];
        e.x      = ScreenToVirtualX(screenX, sw);
        e.y      = ScreenToVirtualY(screenY, sh);
        e.font   = font;
        e.scaleW = scaleW * kTextBoost;
        e.scaleH = scaleH * kTextBoost;
        e.color  = color;
        e.centre = centre;
        e.shadow = 0;   // 22.5.1: no CFont drop shadow either
        e.wrapX  = wrapVirtualX;
        strncpy(e.text, text, sizeof(e.text) - 1);
        e.text[sizeof(e.text) - 1] = '\0';
    } else {
        DrawFallbackText(screenX, screenY, text, font, scaleH, color, centre, shadow);
    }
}

// Flush all queued text through CFont after ImGui has rendered.
void FlushTextQueue() {
    if (!cfont::g_cfontAvailable || g_textQueueCount == 0) return;

    for (int i = 0; i < g_textQueueCount; ++i) {
        const TextEntry& e = g_textQueue[i];
        cfont::RenderText(e.x, e.y, e.text, e.font,
                          e.scaleW, e.scaleH, e.color,
                          e.centre, e.shadow, e.wrapX);
    }
    cfont::FlushFontBuffer();
    g_textQueueCount = 0;
}

// Text centred in a box. CFont keeps its original baseline offset; the ImGui
// fallback centres vertically using the real line height.
void QueueTextInBox(ImVec2 pos, ImVec2 size, const char* text, uint8_t font,
                    float scaleW, float scaleH, cfont::CRGBA color,
                    int16_t shadow = 1, float cfontTopFrac = 0.25f) {
    const float cx = pos.x + size.x * 0.5f;
    if (cfont::g_cfontAvailable) {
        QueueText(cx, pos.y + size.y * cfontTopFrac, text, font, scaleW, scaleH,
                  color, true, shadow);
        return;
    }
    const float px = TextPixelHeight(scaleH);
    if (TextOutsideClip(pos.y, px)) return;
    DrawFallbackText(cx, pos.y + (size.y - px) * 0.5f, text, font, scaleH, color, true, shadow);
}

// Width of a text run at a given CFont-style scale. Measured with the
// fallback face; used for right alignment and footer layout (an estimate
// when CFont is the active renderer).
float TextWidth(const char* text, uint8_t font, float scaleH) {
    if (!text || !*text) return 0.0f;
    return FallbackFont(font)->CalcTextSizeA(TextPixelHeight(scaleH), FLT_MAX, 0.0f, text).x;
}

// Text vertically centred in a box; align 0 = left, 1 = centre, 2 = right.
void QueueTextAligned(ImVec2 pos, ImVec2 size, const char* text, uint8_t font,
                      float scaleW, float scaleH, cfont::CRGBA color,
                      int align, int16_t shadow = 1) {
    const float px = TextPixelHeight(scaleH);
    const float y = pos.y + (size.y - px) * 0.5f;
    if (align == 1)
        QueueText(pos.x + size.x * 0.5f, y, text, font, scaleW, scaleH, color, true, shadow);
    else if (align == 2)
        QueueText(pos.x + size.x - TextWidth(text, font, scaleH), y, text, font,
                  scaleW, scaleH, color, false, shadow);
    else
        QueueText(pos.x, y, text, font, scaleW, scaleH, color, false, shadow);
}

// ═══════════════════════════════════════════════════════════════════════
// SA-MP Data Access (for GAME tab server info)
// ═══════════════════════════════════════════════════════════════════════
namespace samp {

// SA-MP 0.3.7-R5 CNetGame (packed) — deps/omp-cef/deps/samp-api/include/
// sampapi/0.3.7-R5-1/CNetGame.h; RefNetGame() = samp.dll + 0x26EB94.
// Replaces earlier unverified offsets (hostname +0x1022, max players +0x3C8).
constexpr uintptr_t kNetGameRva          = 0x26EB94;
constexpr uintptr_t kHostAddress         = 0x30;    // char[257]
constexpr uintptr_t kHostname            = 0x131;   // char[257]
constexpr uintptr_t kPort                = 0x235;   // int
constexpr uintptr_t kGameState           = 0x3CD;   // int (GameMode)
constexpr uintptr_t kPools               = 0x3DE;   // Pools*
constexpr uintptr_t kPoolsPlayer         = 0x4;     // Pools::m_pPlayer
// CPlayerPool::GetLocalPlayerName — src/sampapi/0.3.7-R5-1/CPlayerPool.cpp
constexpr uintptr_t kGetLocalPlayerName  = 0xA4E0;

enum class Link : uint8_t {
    NoSamp, NoNetGame, WaitConnect, Connecting, Connected, WaitJoin, Restarting, Unknown
};

struct ServerInfo {
    Link  link;
    bool  connected;
    char  hostname[257];
    char  address[257];
    int   port;
    char  nickname[32];
};

ServerInfo GetServerInfo() {
    ServerInfo info{};
    info.link = Link::NoSamp;
    if (!input::IsSampR5Loaded()) return info;

    __try {
        const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleA("samp.dll"));
        auto* netGame = *reinterpret_cast<uint8_t**>(base + kNetGameRva);
        if (!netGame) {
            info.link = Link::NoNetGame;
            return info;
        }
        switch (*reinterpret_cast<int32_t*>(netGame + kGameState)) {
            case 0x1: info.link = Link::WaitConnect; break;
            case 0x2: info.link = Link::Connecting;  break;
            case 0x5: info.link = Link::Connected;   break;
            case 0x6: info.link = Link::WaitJoin;    break;
            case 0xB: info.link = Link::Restarting;  break;
            default:  info.link = Link::Unknown;     break;
        }
        info.connected = info.link == Link::Connected;
        memcpy(info.hostname, netGame + kHostname, sizeof(info.hostname) - 1);
        memcpy(info.address, netGame + kHostAddress, sizeof(info.address) - 1);
        info.port = *reinterpret_cast<int32_t*>(netGame + kPort);

        auto* pools = *reinterpret_cast<uint8_t**>(netGame + kPools);
        void* playerPool = pools ? *reinterpret_cast<void**>(pools + kPoolsPlayer) : nullptr;
        if (playerPool) {
            using GetNameFn = const char*(__thiscall*)(void*);
            const char* name = reinterpret_cast<GetNameFn>(base + kGetLocalPlayerName)(playerPool);
            if (name) strncpy(info.nickname, name, sizeof(info.nickname) - 1);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        info.link = Link::Unknown;
        info.connected = false;
        info.hostname[0] = info.address[0] = info.nickname[0] = '\0';
        info.port = 0;
    }
    return info;
}

} // namespace samp

// ═══════════════════════════════════════════════════════════════════════
// GTA-Style Color Palette
// ═══════════════════════════════════════════════════════════════════════
namespace colors {
    // SKR Enhanced launcher palette — "Block B" (client/lobby/lobby.css:326+,
    // the palette the launcher renders). See docs/ui/SKR_NATIVE_DESIGN_TOKENS.md.
    constexpr ImU32 kBg            = IM_COL32(  7,  11,  17, 255);  // --bg #070b11
    constexpr ImU32 kBgGlow        = IM_COL32( 23,  54,  75, 150);  // body radial #17364b
    constexpr ImU32 kGrid          = IM_COL32(181, 233, 236,   6);  // body:after grid #b5e9ec05
    constexpr ImU32 kTile          = IM_COL32( 16,  24,  33, 255);  // --tile #101821
    constexpr ImU32 kTile2         = IM_COL32( 23,  33,  43, 255);  // --tile2 #17212b
    constexpr ImU32 kPanel         = IM_COL32( 18,  30,  40, 255);  // details panel #121e28
    constexpr ImU32 kPanelBorder   = IM_COL32( 46,  73,  86, 255);  // #2e4956
    constexpr ImU32 kLine          = IM_COL32( 38,  55,  71, 255);  // --line #263747
    constexpr ImU32 kHeaderLine    = IM_COL32( 37,  53,  68, 255);  // header border #253544
    constexpr ImU32 kRowLine       = IM_COL32( 35,  55,  67, 255);  // .setting-row #233743
    constexpr ImU32 kHover         = IM_COL32( 36,  52,  67, 255);  // nav hover #243443
    constexpr ImU32 kAccent        = IM_COL32(168, 238, 101, 255);  // --green #a8ee65
    constexpr ImU32 kAccentSoft    = IM_COL32(168, 238, 101,  22);  // nav active #a8ee6516
    constexpr ImU32 kAccentRing    = IM_COL32(168, 238, 101,  53);  // nav active #a8ee6535
    constexpr ImU32 kPrimary       = IM_COL32(174, 239, 114, 255);  // .button--gold #aeef72
    constexpr ImU32 kPrimaryHover  = IM_COL32(204, 255, 156, 255);  // #ccff9c
    constexpr ImU32 kSecondary     = IM_COL32( 23,  33,  43, 255);  // tile2
    constexpr ImU32 kSecondaryHov  = IM_COL32( 36,  52,  67, 255);  // #243443
    constexpr ImU32 kSecondaryLine = IM_COL32( 52,  78,  92, 255);  // #344e5c
    constexpr ImU32 kDanger        = IM_COL32(240, 138, 131, 255);  // --danger #f08a83
    constexpr ImU32 kDangerBg      = IM_COL32(240, 138, 131,  28);
    constexpr ImU32 kDangerBgHov   = IM_COL32(240, 138, 131,  58);
    constexpr ImU32 kInput         = IM_COL32( 11,  18,  25, 255);  // value boxes
    constexpr ImU32 kSwitchOff     = IM_COL32( 51,  70,  83, 255);  // #334653
    constexpr ImU32 kSwitchOffLine = IM_COL32( 73,  96, 107, 255);  // #49606b
    constexpr ImU32 kSwitchOn      = IM_COL32(103, 143,  76, 255);  // #678f4c
    constexpr ImU32 kSwitchOnLine  = IM_COL32(156, 207, 116, 255);  // #9ccf74
    constexpr ImU32 kKnob          = IM_COL32(210, 224, 229, 255);  // #d2e0e5
    constexpr ImU32 kCategoryOn    = IM_COL32( 33,  57,  50, 255);  // #213932
    constexpr ImU32 kCategoryLine  = IM_COL32( 66, 103,  93, 255);  // #42675d
    constexpr ImU32 kModalBackdrop = IM_COL32(  5,   7,   5, 217);  // #050705d9
    constexpr ImU32 kKbdLine       = IM_COL32( 48,  73,  85, 255);  // #304955
    constexpr ImU32 kBrandA        = IM_COL32(173, 241, 121, 255);  // .brand-symbol #adf179
    constexpr ImU32 kBrandB        = IM_COL32(102, 189, 142, 255);  // .brand-symbol #66bd8e
    constexpr ImU32 kBrandBar      = IM_COL32( 11,  24,  34, 255);  // #0b1822
    constexpr ImU32 kAvatar        = IM_COL32( 33,  57,  50, 255);

    // Semantic (HUD) colours — not brand
    constexpr ImU32 kHealth        = IM_COL32(196,  48,  48, 255);
    constexpr ImU32 kHealthBg      = IM_COL32( 70,  18,  18, 255);
    constexpr ImU32 kArmour        = IM_COL32( 84, 156, 214, 255);
    constexpr ImU32 kArmourBg      = IM_COL32( 24,  48,  70, 255);

    // Text (CRGBA for CFont / fallback)
    constexpr cfont::CRGBA cText       = {244, 248, 250, 255};  // --text #f4f8fa
    constexpr cfont::CRGBA cTitle      = {229, 238, 242, 255};  // .setting-title #e5eef2
    constexpr cfont::CRGBA cMuted      = {148, 163, 178, 255};  // --muted #94a3b2
    constexpr cfont::CRGBA cHint       = {130, 154, 167, 255};  // .setting-hint #829aa7
    constexpr cfont::CRGBA cDim        = {109, 131, 145, 255};  // --dim #6d8391
    constexpr cfont::CRGBA cEyebrow    = {144, 191, 180, 255};  // .eyebrow #90bfb4
    constexpr cfont::CRGBA cTileHead   = {177, 197, 207, 255};  // .tile-heading #b1c5cf
    constexpr cfont::CRGBA cAccent     = {168, 238, 101, 255};  // --green
    constexpr cfont::CRGBA cAccentText = {186, 255, 130, 255};  // nav active #baff82
    constexpr cfont::CRGBA cOnPrimary  = {  9,  23,  27, 255};  // #09171b
    constexpr cfont::CRGBA cDanger     = {240, 138, 131, 255};  // --danger
    constexpr cfont::CRGBA cKbd        = {201, 214, 220, 255};

    // Kept for the remaining focus/slider code
    constexpr ImU32 kTextPrimary     = kKnob;
    constexpr ImU32 kTextAccent      = kAccent;
}

// ═══════════════════════════════════════════════════════════════════════
// State
// ═══════════════════════════════════════════════════════════════════════
bool g_imguiInitialized = false;
bool g_deviceObjectsValid = false;
IDirect3DDevice9* g_device = nullptr;
HWND g_window = nullptr;
float g_menuAlpha = 0.0f;
DWORD g_openTime = 0;

enum class SettingsSection : uint8_t { Display, Graphics, Audio, Controls, Language, Enhanced };
constexpr int kSettingsSectionCount = 6;
SettingsSection g_settingsSection = SettingsSection::Display;

// ── Tab names ───────────────────────────────────────────────────────
const char* kTabNames[] = { "MAP", "GAME", "SETTINGS", "SERVERS", "ABOUT" };
constexpr int kTabCount = 5;

// ═══════════════════════════════════════════════════════════════════════
// Style Setup (minimal — we draw most things manually via DrawList)
// ═══════════════════════════════════════════════════════════════════════
void ApplyGtaStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.ChildRounding  = 0.0f;
    style.FrameRounding  = 2.0f;
    style.PopupRounding  = 0.0f;
    style.TabRounding    = 0.0f;
    style.WindowPadding  = ImVec2(0, 0);
    style.FramePadding   = ImVec2(0, 0);
    style.ItemSpacing    = ImVec2(0, 0);
    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize  = 0.0f;
    style.FrameBorderSize  = 0.0f;

    ImVec4* c = style.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0); // Transparent — we draw our own bg
}

// ═══════════════════════════════════════════════════════════════════════
// Screen Helpers
// ═══════════════════════════════════════════════════════════════════════
// Layout uses ImGui's display size when available: it is the coordinate
// space of both the draw data and the Win32 mouse position, so hitboxes stay
// correct even if the back buffer and client area differ. The D3D9 viewport
// is the fallback before the first ImGui frame.
float GetScreenWidth() {
    if (ImGui::GetCurrentContext() && ImGui::GetIO().DisplaySize.x > 0.0f)
        return ImGui::GetIO().DisplaySize.x;
    D3DVIEWPORT9 vp;
    if (g_device && SUCCEEDED(g_device->GetViewport(&vp)))
        return static_cast<float>(vp.Width);
    return 800.0f;
}

float GetScreenHeight() {
    if (ImGui::GetCurrentContext() && ImGui::GetIO().DisplaySize.y > 0.0f)
        return ImGui::GetIO().DisplaySize.y;
    D3DVIEWPORT9 vp;
    if (g_device && SUCCEEDED(g_device->GetViewport(&vp)))
        return static_cast<float>(vp.Height);
    return 600.0f;
}

// ═══════════════════════════════════════════════════════════════════════
// Resolution-Aware Layout System
// ═══════════════════════════════════════════════════════════════════════
// Computed once per frame from the D3D9 viewport.  Every position, size,
// padding and font scale is derived from this struct — nothing is
// hardcoded to a specific resolution.

struct LayoutInfo {
    float sw, sh;                // viewport pixels
    float aspectRatio;           // sw / sh
    float dpiScale;              // sh / 1080  (resolution factor)
    float uiScale;               // user multiplier (0.75 – 1.50)
    float scale;                 // dpiScale * uiScale (combined)

    // Full-screen panel
    float panelX, panelY;
    float panelW, panelH;

    // Content area (horizontally centred, clamped for ultrawide)
    float contentX;
    float contentW;
    float contentMaxW;           // max before clamping

    // Padding
    float padX;                  // horizontal padding inside content area
    float padY;                  // vertical padding

    // Font scale multiplier for CFont (applied on top of per-call values)
    // CFont already scales with resolution (640×448 virtual space maps to
    // the full viewport), so this carries only the user's UI-scale pref
    // and a floor for very low resolutions.
    float fontFactor;

    // Convenience
    float S(float base) const { return base * scale; }
};

static LayoutInfo g_layout = {};

static void ComputeLayout() {
    LayoutInfo& L = g_layout;
    L.sw = GetScreenWidth();
    L.sh = GetScreenHeight();
    L.aspectRatio = (L.sh > 0.0f) ? (L.sw / L.sh) : (16.0f / 9.0f);
    L.dpiScale = L.sh / 1080.0f;
    L.uiScale  = settings::GetUiScale();
    L.scale    = L.dpiScale * L.uiScale;

    // Full-screen panel (covers entire viewport)
    L.panelX = 0.0f;
    L.panelY = 0.0f;
    L.panelW = L.sw;
    L.panelH = L.sh;

    // Full-width shell like the launcher (.shell padding clamp(22px, 3vw, 62px));
    // pages split this region into adaptive columns instead of a fixed page width.
    const float shellPad = std::clamp(L.sw * 0.03f, L.S(16.0f), L.S(62.0f));
    L.contentMaxW = L.sw - shellPad * 2.0f;
    L.contentW    = L.contentMaxW;
    L.contentX    = shellPad;

    // Padding inside panels
    L.padX = L.S(28.0f);
    L.padY = L.S(10.0f);

    // Font factor: user UI scale, with a floor so text stays readable
    // at very low resolutions (e.g. 640×480).
    // At 480p dpiScale ≈ 0.44; floor ensures fontFactor ≥ 0.60.
    L.fontFactor = fmaxf(L.uiScale, 0.60f / fmaxf(L.dpiScale, 0.01f));
    // But also cap it so we don't blow up at huge scales
    L.fontFactor = fminf(L.fontFactor, 2.0f);

    // Recomputed every frame so resolution changes / device resets apply.
    ImGui::GetIO().FontGlobalScale = fmaxf(L.dpiScale * L.uiScale, 0.75f);
}

// Scale relative to 1080p reference, incorporating user UI scale
float Scale(float base) {
    return base * g_layout.scale;
}

// ═══════════════════════════════════════════════════════════════════════
// Keyboard Navigation + Scroll State
// ═══════════════════════════════════════════════════════════════════════
// Immediate-mode focus: every navigable control calls NavItem() in draw
// order; Up/Down move the focus index, Left/Right adjust the focused
// control, Enter/Space activate it. Focus starts at the first control of a
// tab so Enter keeps its old meaning (MAP opens the map, GAME resumes).
// The highlight is shown only after a key press; moving the mouse hides it.
struct NavState {
    int    tab = -1;
    int    focus = 0;
    int    count = 0;
    int    lastCount = 0;
    int    adjust = 0;
    bool   activate = false;
    bool   keyboard = false;
    bool   focusSeen = false;
    ImVec2 focusMin;
    ImVec2 focusMax;
};
NavState g_nav;

float g_tabScroll[kTabCount] = {};
float g_tabExtent[kTabCount] = {};

void BeginNav(int tab) {
    if (tab != g_nav.tab) {
        g_nav.tab = tab;
        g_nav.focus = 0;
        g_nav.keyboard = false;
    }
    g_nav.adjust = 0;
    g_nav.activate = false;
    for (;;) {
        const input::NavEvent ev = input::PopNavEvent();
        if (ev == input::NavEvent::None) break;
        g_nav.keyboard = true;
        const int n = g_nav.lastCount;
        switch (ev) {
            case input::NavEvent::Up:
                if (n > 0) g_nav.focus = (g_nav.focus <= 0) ? n - 1 : g_nav.focus - 1;
                break;
            case input::NavEvent::Down:
                if (n > 0) g_nav.focus = (g_nav.focus >= n - 1) ? 0 : g_nav.focus + 1;
                break;
            case input::NavEvent::Left:     g_nav.adjust -= 1;     break;
            case input::NavEvent::Right:    g_nav.adjust += 1;     break;
            case input::NavEvent::Activate: g_nav.activate = true; break;
            default: break;
        }
    }
    const ImVec2 delta = ImGui::GetIO().MouseDelta;
    if (delta.x != 0.0f || delta.y != 0.0f) g_nav.keyboard = false;
    g_nav.count = 0;
    g_nav.focusSeen = false;
}

void EndNav() {
    g_nav.lastCount = g_nav.count;
    if (g_nav.count == 0) g_nav.focus = 0;
    else if (g_nav.focus >= g_nav.count) g_nav.focus = g_nav.count - 1;
}

// ── Confirmation dialogs ────────────────────────────────────────────
// While a dialog is open the page underneath is drawn but inert: no hover,
// clicks, keyboard focus, wheel scrolling or tab switching.
enum class Modal : uint8_t {
    None, ExitGame, Disconnect,
    RestoreDisplay, RestoreAudio, RestoreControls,   // Milestone B
    BindingConflict,                                  // native controls editor
    ResetEnhanced,                                    // Milestone C
    VideoConfirm,                                     // keep / revert resolution
};
Modal g_modal = Modal::None;
int   g_modalFocus = 0;      // 0 = cancel (default), 1 = confirm
bool  g_uiBlocked = false;

void OpenModal(Modal m) {
    g_modal = m;
    g_modalFocus = 0;
}

bool HoverRect(ImVec2 min, ImVec2 max, bool clip = true) {
    return !g_uiBlocked && ImGui::IsMouseHoveringRect(min, max, clip);
}

bool NavItem(ImVec2 min, ImVec2 max) {
    const int index = g_nav.count++;
    if (g_uiBlocked) return false;
    if (index != g_nav.focus) return false;
    g_nav.focusSeen = true;
    g_nav.focusMin = min;
    g_nav.focusMax = max;
    return true;
}

bool NavHighlight(bool focused) {
    return focused && g_nav.keyboard;
}

void DrawRowFocus(ImDrawList* dl, ImVec2 min, ImVec2 max, bool focused, bool hovered) {
    // Launcher .category.active: tinted fill + 3px inset accent bar.
    if (NavHighlight(focused)) {
        dl->AddRectFilled(min, max, colors::kAccentSoft, Scale(6.0f));
        dl->AddRectFilled(min, ImVec2(min.x + Scale(3.0f), max.y), colors::kAccent);
    } else if (hovered) {
        dl->AddRectFilled(min, max, IM_COL32(255, 255, 255, 8), Scale(6.0f));
    }
}

// ═══════════════════════════════════════════════════════════════════════
// GTA-Style Button (ImGui rect for bg + hit testing, CFont for text)
// ═══════════════════════════════════════════════════════════════════════
// Returns true if clicked.
bool DrawGtaButton(ImDrawList* dl, ImVec2 pos, ImVec2 size, const char* label,
                   ImU32 bgNormal, ImU32 bgHover, uint8_t font = cfont::FONT_MENU,
                   float fontScaleW = 0.45f, float fontScaleH = 0.90f,
                   cfont::CRGBA textColor = colors::cText, bool navigable = true,
                   ImU32 border = 0) {
    const ImVec2 maxPt(pos.x + size.x, pos.y + size.y);
    // Clip-aware: controls scrolled out of the content view are not clickable.
    const bool hovered = HoverRect(pos, maxPt);
    const bool focused = navigable && NavItem(pos, maxPt);
    const bool clicked = (hovered && ImGui::IsMouseClicked(0)) ||
                         (focused && g_nav.activate);

    const float r = Scale(6.0f);   // launcher buttons: border-radius 5–7px
    dl->AddRectFilled(pos, maxPt, (hovered || NavHighlight(focused)) ? bgHover : bgNormal, r);
    if (border) dl->AddRect(pos, maxPt, border, r, 0, 1.0f);
    if (NavHighlight(focused)) {
        // Launcher focus-visible: 2px accent outline with a small offset.
        const float o = Scale(3.0f);
        dl->AddRect(ImVec2(pos.x - o, pos.y - o), ImVec2(maxPt.x + o, maxPt.y + o),
                    colors::kAccent, r + o, 0, 2.0f);
    }

    // Font factor carries the UI scale preference.
    const float ff = g_layout.fontFactor;
    QueueTextInBox(pos, size, label, font, fontScaleW * ff, fontScaleH * ff, textColor);
    return clicked;
}

// ═══════════════════════════════════════════════════════════════════════
// Milestone A.1 — Type Scale and Row Layout
// ═══════════════════════════════════════════════════════════════════════
// CFont-style scales (before kTextBoost and the UI-scale font factor).
// At 1366x768 with UI scale 1.0 the fallback renders kLabel at ~18 px.
struct TextStyle { uint8_t font; float w; float h; };
namespace ts {
    constexpr TextStyle kTitle   = { cfont::FONT_GOTHIC,    0.72f, 1.40f };
    constexpr TextStyle kTab     = { cfont::FONT_MENU,      0.42f, 0.86f };
    constexpr TextStyle kSubTab  = { cfont::FONT_MENU,      0.34f, 0.70f };
    constexpr TextStyle kHeading = { cfont::FONT_GOTHIC,    0.52f, 1.00f };
    constexpr TextStyle kLabel   = { cfont::FONT_SUBTITLES, 0.34f, 0.72f };
    constexpr TextStyle kValue   = { cfont::FONT_SUBTITLES, 0.32f, 0.68f };
    constexpr TextStyle kBody    = { cfont::FONT_SUBTITLES, 0.34f, 0.72f };
    constexpr TextStyle kHint    = { cfont::FONT_SUBTITLES, 0.28f, 0.58f };
    constexpr TextStyle kButton  = { cfont::FONT_MENU,      0.30f, 0.62f };
    constexpr TextStyle kButtonL = { cfont::FONT_MENU,      0.38f, 0.78f };
    constexpr TextStyle kKey     = { cfont::FONT_MENU,      0.26f, 0.54f };
    constexpr TextStyle kNav     = { cfont::FONT_MENU,      0.36f, 0.74f };  // launcher .nav-button
    constexpr TextStyle kEyebrow = { cfont::FONT_MENU,      0.22f, 0.46f };  // .eyebrow
    constexpr TextStyle kPage    = { cfont::FONT_GOTHIC,    0.62f, 1.18f };  // page h1 (Bahnschrift)
    constexpr TextStyle kPanelH  = { cfont::FONT_MENU,      0.30f, 0.62f };  // .tile-heading
}

void TextAt(float x, float y, const char* text, const TextStyle& st,
            cfont::CRGBA color, bool centre = false, int16_t shadow = 1) {
    const float ff = g_layout.fontFactor;
    QueueText(x, y, text, st.font, st.w * ff, st.h * ff, color, centre, shadow);
}

void TextIn(ImVec2 pos, ImVec2 size, const char* text, const TextStyle& st,
            cfont::CRGBA color, int align, int16_t shadow = 1) {
    const float ff = g_layout.fontFactor;
    QueueTextAligned(pos, size, text, st.font, st.w * ff, st.h * ff, color, align, shadow);
}

float TextW(const char* text, const TextStyle& st) {
    return TextWidth(text, st.font, st.h * g_layout.fontFactor);
}

// SA-MP strings are in the system ANSI code page. ImGui expects UTF-8; CFont
// takes the raw bytes, so convert only for the ImGui fallback path.
void ToDisplayText(const char* in, char* out, size_t cap) {
    if (!cap) return;
    out[0] = '\0';
    if (!in) return;
    if (cfont::g_cfontAvailable) {
        strncpy(out, in, cap - 1);
        out[cap - 1] = '\0';
        return;
    }
    wchar_t wide[512];
    const int n = MultiByteToWideChar(CP_ACP, 0, in, -1, wide, 512);
    if (n <= 0 || WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, static_cast<int>(cap),
                                      nullptr, nullptr) <= 0) {
        strncpy(out, in, cap - 1);
        out[cap - 1] = '\0';
    }
}

// Truncates with "..." until the text fits maxW (UTF-8 safe).
void FitText(char* buf, size_t cap, const TextStyle& st, float maxW) {
    if (TextW(buf, st) <= maxW) return;
    size_t n = strlen(buf);
    while (n > 0) {
        --n;
        while (n > 0 && (static_cast<unsigned char>(buf[n]) & 0xC0) == 0x80) --n;
        if (n + 4 > cap) continue;
        memcpy(buf + n, "...", 4);
        if (TextW(buf, st) <= maxW) return;
    }
}

// Launcher button roles: primary (.button--gold → green, dark text),
// secondary (tile2 + 1px line), danger (destructive: red tint + red text).
enum class Btn : uint8_t { Primary, Secondary, Danger };

bool StyledButton(ImDrawList* dl, ImVec2 pos, ImVec2 size, const char* label,
                  Btn kind, const TextStyle& st, bool navigable = true) {
    switch (kind) {
        case Btn::Primary:
            return DrawGtaButton(dl, pos, size, label, colors::kPrimary, colors::kPrimaryHover,
                                 st.font, st.w, st.h, colors::cOnPrimary, navigable);
        case Btn::Danger:
            return DrawGtaButton(dl, pos, size, label, colors::kDangerBg, colors::kDangerBgHov,
                                 st.font, st.w, st.h, colors::cDanger, navigable, colors::kDanger);
        default:
            return DrawGtaButton(dl, pos, size, label, colors::kSecondary, colors::kSecondaryHov,
                                 st.font, st.w, st.h, colors::cTitle, navigable,
                                 colors::kSecondaryLine);
    }
}

// One settings row (launcher .setting-row): label left, control column right
// with a bounded width so controls never stretch across wide screens.
float RowHeight()     { return Scale(56.0f); }
float ControlHeight() { return Scale(38.0f); }

struct RowGeom {
    ImVec2 min, max;
    float labelX;
    float ctrlX, ctrlW, ctrlY, ctrlH;
};

RowGeom MakeRow(float x, float y, float width) {
    RowGeom g{};
    const float rowH = RowHeight();
    g.min = ImVec2(x, y);
    g.max = ImVec2(x + width, y + rowH);
    g.labelX = x + Scale(16.0f);
    g.ctrlW = std::clamp(width * 0.42f, Scale(260.0f), Scale(460.0f));
    g.ctrlW = fminf(g.ctrlW, width * 0.6f);
    g.ctrlX = x + width - Scale(16.0f) - g.ctrlW;
    g.ctrlH = ControlHeight();
    g.ctrlY = y + (rowH - g.ctrlH) * 0.5f;
    return g;
}

bool BeginRow(ImDrawList* dl, const RowGeom& g, const char* label) {
    const bool focused = NavItem(g.min, g.max);
    const bool hovered = HoverRect(g.min, g.max);
    DrawRowFocus(dl, g.min, g.max, focused, hovered);
    dl->AddLine(ImVec2(g.min.x, g.max.y), ImVec2(g.max.x, g.max.y), colors::kRowLine, 1.0f);
    TextIn(ImVec2(g.labelX, g.min.y),
           ImVec2(fmaxf(0.0f, g.ctrlX - g.labelX - Scale(12.0f)), g.max.y - g.min.y),
           label, ts::kLabel,
           (NavHighlight(focused) || hovered) ? colors::cText : colors::cTitle, 0);
    return focused;
}

// "< value >" with optional APPLY (primary). Reports a step and an apply request.
void DrawSettingStepper(ImDrawList* dl, float x, float y, float width, const char* label,
                        const char* valueText, bool hasApply, int& step, bool& apply) {
    step = 0;
    apply = false;
    const RowGeom g = MakeRow(x, y, width);
    const bool focused = BeginRow(dl, g, label);
    const float arrowW = g.ctrlH;
    const float gap = Scale(8.0f);
    float right = g.ctrlX + g.ctrlW;
    if (hasApply) {
        const float applyW = Scale(110.0f);
        right -= applyW;
        if (StyledButton(dl, ImVec2(right, g.ctrlY), ImVec2(applyW, g.ctrlH), "APPLY",
                         Btn::Primary, ts::kButton, false))
            apply = true;
        right -= gap;
    }
    if (StyledButton(dl, ImVec2(g.ctrlX, g.ctrlY), ImVec2(arrowW, g.ctrlH), "<",
                     Btn::Secondary, ts::kButton, false))
        step = -1;
    if (StyledButton(dl, ImVec2(right - arrowW, g.ctrlY), ImVec2(arrowW, g.ctrlH), ">",
                     Btn::Secondary, ts::kButton, false))
        step = 1;
    const ImVec2 boxMin(g.ctrlX + arrowW + Scale(4.0f), g.ctrlY);
    const ImVec2 boxMax(right - arrowW - Scale(4.0f), g.ctrlY + g.ctrlH);
    dl->AddRectFilled(boxMin, boxMax, colors::kInput, Scale(5.0f));
    dl->AddRect(boxMin, boxMax, colors::kLine, Scale(5.0f), 0, 1.0f);
    // Long values (e.g. "1366 x 768 x 32 windowed") step down one size to fit.
    const bool fits = TextW(valueText, ts::kValue) <= (boxMax.x - boxMin.x) - Scale(12.0f);
    TextIn(boxMin, ImVec2(boxMax.x - boxMin.x, g.ctrlH), valueText,
           fits ? ts::kValue : ts::kHint, colors::cText, 1);
    if (focused && g_nav.adjust != 0) step = g_nav.adjust;
    if (focused && g_nav.activate && hasApply) apply = true;
}

// Row with a single action button in the control column (restore defaults,
// vanilla fallbacks). Enter on the focused row activates the button.
bool DrawActionRow(ImDrawList* dl, float x, float y, float width, const char* label,
                   const char* button, Btn kind) {
    const RowGeom g = MakeRow(x, y, width);
    const bool focused = BeginRow(dl, g, label);
    const float bw = fminf(g.ctrlW, Scale(240.0f));
    const bool clicked = StyledButton(dl, ImVec2(g.ctrlX + g.ctrlW - bw, g.ctrlY),
                                      ImVec2(bw, g.ctrlH), button, kind, ts::kButton, false);
    return clicked || (focused && g_nav.activate);
}

// Native GTA binding row. The three existing secondary-button variants are
// used for keyboard primary, keyboard secondary, and mouse; no new visual
// system is introduced. Keyboard navigation uses Left/Right to choose a
// column and Enter to begin capture.
void DrawBindingRow(ImDrawList* dl, float x, float y, float width,
                    const controls::ActionDescriptor& action) {
    static int keyboardColumn = 0;
    const RowGeom g = MakeRow(x, y, width);
    const bool focused = BeginRow(dl, g, action.label);
    if (!action.editable) {
        TextIn(ImVec2(g.ctrlX, g.min.y), ImVec2(g.ctrlW, g.max.y - g.min.y),
               "READ ONLY", ts::kHint, colors::cHint, 2);
        return;
    }

    if (focused && g_nav.adjust != 0)
        keyboardColumn = std::clamp(keyboardColumn + g_nav.adjust, 0, 2);

    constexpr controls::BindingSlot slots[] = {
        controls::BindingSlot::Primary,
        controls::BindingSlot::Secondary,
        controls::BindingSlot::Mouse,
    };
    const char* prefixes[] = { "1 ", "2 ", "M " };
    const float gap = Scale(6.0f);
    const float bw = (g.ctrlW - gap * 2.0f) / 3.0f;
    for (int i = 0; i < 3; ++i) {
        char binding[48]{};
        char button[56]{};
        controls::FormatBinding(action.id, slots[i], binding, sizeof(binding));
        snprintf(button, sizeof(button), "%s%s", prefixes[i], binding);
        FitText(button, sizeof(button), ts::kHint, bw - Scale(10.0f));
        const ImVec2 pos(g.ctrlX + i * (bw + gap), g.ctrlY);
        const bool clicked = StyledButton(dl, pos, ImVec2(bw, g.ctrlH), button,
                                          Btn::Secondary, ts::kHint, false);
        if (clicked || (focused && g_nav.activate && keyboardColumn == i))
            controls::BeginCapture(action.id, slots[i]);
        if (focused && keyboardColumn == i && g_nav.keyboard)
            dl->AddRect(pos, ImVec2(pos.x + bw, pos.y + g.ctrlH), colors::kAccent,
                        Scale(5.0f), 0, 2.0f);
    }
}

// Settings category (launcher .category): number + name + hint; active =
// green-tinted fill, border and 3px inset accent bar.
bool DrawCategory(ImDrawList* dl, ImVec2 pos, ImVec2 size, const char* number,
                  const char* name, const char* hint, bool active) {
    const ImVec2 maxPt(pos.x + size.x, pos.y + size.y);
    const bool hovered = HoverRect(pos, maxPt);
    const bool focused = NavItem(pos, maxPt);
    const bool clicked = (hovered && ImGui::IsMouseClicked(0)) || (focused && g_nav.activate);
    const float r = Scale(9.0f);
    if (active) {
        dl->AddRectFilled(pos, maxPt, colors::kCategoryOn, r);
        dl->AddRect(pos, maxPt, colors::kCategoryLine, r, 0, 1.0f);
        dl->AddRectFilled(pos, ImVec2(pos.x + Scale(3.0f), maxPt.y), colors::kAccent);
    } else if (hovered || NavHighlight(focused)) {
        dl->AddRectFilled(pos, maxPt, colors::kHover, r);
    }
    if (NavHighlight(focused))
        dl->AddRect(pos, maxPt, colors::kAccent, r, 0, 2.0f);
    const float px = Scale(14.0f);
    const float numW = Scale(34.0f);
    TextIn(ImVec2(pos.x + px, pos.y), ImVec2(numW, size.y), number, ts::kEyebrow,
           colors::cDim, 0, 0);
    const float tx = pos.x + px + numW;
    const float tw = size.x - px * 2.0f - numW;
    TextIn(ImVec2(tx, pos.y + size.y * 0.12f), ImVec2(tw, size.y * 0.45f), name, ts::kLabel,
           active ? colors::cAccentText : colors::cTitle, 0);
    TextIn(ImVec2(tx, pos.y + size.y * 0.52f), ImVec2(tw, size.y * 0.36f), hint, ts::kHint,
           colors::cHint, 0, 0);
    return clicked;
}

// Compact sub-tab (narrow screens, SETTINGS sections).
bool DrawSubTab(ImDrawList* dl, ImVec2 pos, ImVec2 size, const char* label, bool active) {
    const ImVec2 maxPt(pos.x + size.x, pos.y + size.y);
    const bool hovered = HoverRect(pos, maxPt);
    const bool focused = NavItem(pos, maxPt);
    const bool clicked = (hovered && ImGui::IsMouseClicked(0)) || (focused && g_nav.activate);
    const float r = Scale(7.0f);
    if (active) {
        dl->AddRectFilled(pos, maxPt, colors::kAccentSoft, r);
        dl->AddRect(pos, maxPt, colors::kAccentRing, r, 0, 1.0f);
    } else if (hovered || NavHighlight(focused)) {
        dl->AddRectFilled(pos, maxPt, colors::kHover, r);
    }
    if (NavHighlight(focused))
        dl->AddRect(pos, maxPt, colors::kAccent, r, 0, 2.0f);
    TextIn(pos, size, label, ts::kSubTab,
           active ? colors::cAccentText : (hovered ? colors::cText : colors::cMuted), 1, 0);
    return clicked;
}

bool DrawSettingToggle(ImDrawList* dl, float x, float y, float width,
                       const char* label, bool value) {
    const RowGeom g = MakeRow(x, y, width);
    const bool focused = BeginRow(dl, g, label);
    // Launcher .switch: state text + 43x24 pill, right-aligned in the control column.
    const float sw = Scale(48.0f), sh = Scale(26.0f);
    const ImVec2 p0(g.ctrlX + g.ctrlW - sw, g.min.y + ((g.max.y - g.min.y) - sh) * 0.5f);
    const ImVec2 p1(p0.x + sw, p0.y + sh);
    const bool hovered = HoverRect(p0, p1);
    dl->AddRectFilled(p0, p1, value ? colors::kSwitchOn : colors::kSwitchOff, sh * 0.5f);
    dl->AddRect(p0, p1, value ? colors::kSwitchOnLine : colors::kSwitchOffLine, sh * 0.5f, 0, 1.0f);
    const float kr = sh * 0.5f - Scale(4.0f);
    const float kx = value ? p1.x - Scale(4.0f) - kr : p0.x + Scale(4.0f) + kr;
    dl->AddCircleFilled(ImVec2(kx, p0.y + sh * 0.5f), kr, colors::kKnob);
    const float stateW = Scale(70.0f);
    TextIn(ImVec2(p0.x - Scale(12.0f) - stateW, g.min.y), ImVec2(stateW, g.max.y - g.min.y),
           value ? "ON" : "OFF", ts::kValue, value ? colors::cAccent : colors::cMuted, 2);
    // Click anywhere on the switch or its state text toggles.
    const ImVec2 hit0(p0.x - Scale(12.0f) - stateW, g.ctrlY);
    const bool clicked = (hovered || HoverRect(hit0, ImVec2(p1.x, g.ctrlY + g.ctrlH))) &&
                         ImGui::IsMouseClicked(0);
    return clicked || (focused && (g_nav.activate || g_nav.adjust != 0));
}

bool DrawSettingSlider(ImDrawList* dl, const char* id, float x, float y, float width,
                       const char* label, float& value, float minValue, float maxValue,
                       const char* valueText, bool& committed) {
    const RowGeom g = MakeRow(x, y, width);
    const bool focused = BeginRow(dl, g, label);

    const float valueW = Scale(100.0f);
    const float trackX = g.ctrlX + Scale(10.0f);
    const float trackW = fmaxf(Scale(60.0f), g.ctrlW - valueW - Scale(24.0f));
    const float trackY = g.min.y + (g.max.y - g.min.y) * 0.5f;
    const float hitH = g.ctrlH;
    ImGui::SetCursorScreenPos(ImVec2(trackX - Scale(10.0f), trackY - hitH * 0.5f));
    ImGui::InvisibleButton(id, ImVec2(trackW + Scale(20.0f), hitH));

    const bool active = ImGui::IsItemActive();
    bool changed = false;
    if (active && ImGui::IsMouseDown(0)) {
        const float mouseX = ImGui::GetIO().MousePos.x;
        const float t = std::clamp((mouseX - trackX) / trackW, 0.0f, 1.0f);
        const float next = minValue + (maxValue - minValue) * t;
        if (std::fabs(next - value) > 0.000001f) {
            value = next;
            changed = true;
        }
    }
    committed = ImGui::IsItemDeactivated();
    if (focused && g_nav.adjust != 0) {
        // Keyboard: 1/32 of the range per step, persisted immediately.
        const float step = (maxValue - minValue) / 32.0f;
        const float next = std::clamp(value + step * static_cast<float>(g_nav.adjust),
                                      minValue, maxValue);
        if (next != value) {
            value = next;
            changed = true;
            committed = true;
        }
    }

    // Launcher range: neutral track, accent fill, value text in accent.
    const float t = std::clamp((value - minValue) / (maxValue - minValue), 0.0f, 1.0f);
    const float half = fmaxf(2.0f, Scale(2.5f));
    dl->AddRectFilled(ImVec2(trackX, trackY - half), ImVec2(trackX + trackW, trackY + half),
                      colors::kSwitchOff, half);
    dl->AddRectFilled(ImVec2(trackX, trackY - half), ImVec2(trackX + trackW * t, trackY + half),
                      colors::kAccent, half);
    const float knobR = fmaxf(5.0f, Scale(9.0f));
    const ImVec2 knob(trackX + trackW * t, trackY);
    if (active || NavHighlight(focused))
        dl->AddCircleFilled(knob, knobR + Scale(4.0f), colors::kAccentRing);
    dl->AddCircleFilled(knob, knobR, colors::kKnob);
    TextIn(ImVec2(g.ctrlX + g.ctrlW - valueW, g.min.y), ImVec2(valueW, g.max.y - g.min.y),
           valueText, ts::kValue, colors::cAccent, 2);
    return changed;
}

// ═══════════════════════════════════════════════════════════════════════
// Tab Content Renderers
// ═══════════════════════════════════════════════════════════════════════


// ── Page composition (launcher .page-heading / panels) ──────────────
// Eyebrow + large title with the launcher's accent title-dot. Returns height.
float PageHeading(float x, float y, float w, const char* eyebrow, const char* title,
                  const char* subtitle = nullptr) {
    const float eyebrowH = Scale(26.0f);
    TextIn(ImVec2(x, y), ImVec2(w, eyebrowH), eyebrow, ts::kEyebrow, colors::cEyebrow, 0, 0);
    const float titleH = Scale(62.0f);
    const float ty = y + eyebrowH;
    TextIn(ImVec2(x, ty), ImVec2(w, titleH), title, ts::kPage, colors::cText, 0);
    const float tw = TextW(title, ts::kPage);
    TextIn(ImVec2(x + tw + Scale(2.0f), ty), ImVec2(Scale(30.0f), titleH), ".", ts::kPage,
           colors::cAccent, 0);
    float h = eyebrowH + titleH;
    if (subtitle) {
        TextIn(ImVec2(x, y + h), ImVec2(w, Scale(32.0f)), subtitle, ts::kBody, colors::cMuted, 0);
        h += Scale(32.0f);
    }
    return h;
}

void DrawPanel(ImDrawList* dl, ImVec2 a, ImVec2 b) {
    const float r = Scale(12.0f);   // .server-details__panel radius 12
    dl->AddRectFilled(a, b, colors::kPanel, r);
    dl->AddRect(a, b, colors::kPanelBorder, r, 0, 1.0f);
}

// Two-column split of a content region; narrow regions stack.
struct Columns { bool wide; float lx, lw, rx, rw; };
Columns SplitColumns(ImVec2 pos, ImVec2 size, float leftFrac, float leftMin, float leftMax,
                     float wideMin) {
    Columns c{};
    c.wide = size.x >= wideMin;
    const float gap = Scale(40.0f);
    if (c.wide) {
        c.lw = std::clamp(size.x * leftFrac, leftMin, leftMax);
        c.lx = pos.x;
        c.rx = pos.x + c.lw + gap;
        c.rw = size.x - c.lw - gap;
    } else {
        c.lx = c.rx = pos.x;
        c.lw = c.rw = size.x;
    }
    return c;
}

// Card with a heading and label/value rows.
struct InfoRow { const char* label; const char* value; cfont::CRGBA color; };

float CardHeight(int count, bool note) {
    return Scale(58.0f) + Scale(44.0f) * count + (note ? Scale(44.0f) : 0.0f) + Scale(18.0f);
}

float DrawInfoCard(ImDrawList* dl, float x, float y, float w, const char* heading,
                   const InfoRow* rows, int count, const char* note = nullptr) {
    const float pad = Scale(24.0f);
    const float headH = Scale(58.0f);
    const float rowH = Scale(44.0f);
    const float noteH = note ? Scale(44.0f) : 0.0f;
    const float h = CardHeight(count, note != nullptr);
    DrawPanel(dl, ImVec2(x, y), ImVec2(x + w, y + h));
    TextIn(ImVec2(x + pad, y), ImVec2(w - pad * 2.0f, headH), heading, ts::kPanelH,
           colors::cTileHead, 0, 0);
    dl->AddLine(ImVec2(x + pad, y + headH), ImVec2(x + w - pad, y + headH),
                colors::kRowLine, 1.0f);
    const float labelW = fminf(Scale(170.0f), (w - pad * 2.0f) * 0.4f);
    float ry = y + headH + Scale(8.0f);
    for (int i = 0; i < count; ++i) {
        TextIn(ImVec2(x + pad, ry), ImVec2(labelW, rowH), rows[i].label, ts::kLabel,
               colors::cHint, 0);
        char value[300];
        strncpy(value, rows[i].value ? rows[i].value : "", sizeof(value) - 1);
        value[sizeof(value) - 1] = '\0';
        const float valueW = w - pad * 2.0f - labelW;
        FitText(value, sizeof(value), ts::kValue, valueW);
        TextIn(ImVec2(x + pad + labelW, ry), ImVec2(valueW, rowH), value, ts::kValue,
               rows[i].color, 0);
        ry += rowH;
    }
    if (note)
        TextIn(ImVec2(x + pad, ry), ImVec2(w - pad * 2.0f, noteH), note, ts::kHint,
               colors::cHint, 0);
    return h;
}

// PLAYER card: nickname + live health/armour from the verified CPed fields.
// Inner padding ~20–24 px (never below 20 px x UI scale) and slightly taller
// rows; still shorter than the SESSION card beside it.
float VitalsPad()       { return fmaxf(Scale(24.0f), 20.0f * g_layout.uiScale); }
float VitalsRowH()      { return Scale(50.0f); }
float VitalsCardHeight() { return Scale(58.0f) + VitalsRowH() * 3 + Scale(24.0f); }

float DrawVitalsCard(ImDrawList* dl, float x, float y, float w, const char* nickname) {
    const float pad = VitalsPad();
    const float headH = Scale(58.0f);
    const float rowH = VitalsRowH();
    const float h = VitalsCardHeight();
    DrawPanel(dl, ImVec2(x, y), ImVec2(x + w, y + h));
    TextIn(ImVec2(x + pad, y), ImVec2(w - pad * 2.0f, headH), "PLAYER", ts::kPanelH,
           colors::cTileHead, 0, 0);
    dl->AddLine(ImVec2(x + pad, y + headH), ImVec2(x + w - pad, y + headH), colors::kRowLine, 1.0f);
    const float labelW = fminf(Scale(170.0f), (w - pad * 2.0f) * 0.4f);
    float ry = y + headH + Scale(12.0f);

    TextIn(ImVec2(x + pad, ry), ImVec2(labelW, rowH), "Nickname", ts::kLabel, colors::cHint, 0);
    TextIn(ImVec2(x + pad + labelW, ry), ImVec2(w - pad * 2.0f - labelW, rowH),
           (nickname && nickname[0]) ? nickname : "Unknown", ts::kValue, colors::cText, 0);
    ry += rowH;

    float health = 0.0f, maxHealth = 100.0f, armour = 0.0f;
    const bool ok = skr::radar_hud::ReadPlayerVitals(health, maxHealth, armour);
    struct Bar { const char* label; float value; float max; ImU32 fg; ImU32 bg; };
    const Bar bars[2] = {
        { "Health", health, maxHealth, colors::kHealth, colors::kHealthBg },
        { "Armour", armour, 100.0f,    colors::kArmour, colors::kArmourBg },
    };
    for (const Bar& b : bars) {
        TextIn(ImVec2(x + pad, ry), ImVec2(labelW, rowH), b.label, ts::kLabel, colors::cHint, 0);
        const float numW = Scale(56.0f);
        const float bx = x + pad + labelW;
        const float bw = fmaxf(Scale(40.0f), w - pad * 2.0f - labelW - numW - Scale(12.0f));
        const float bh = Scale(8.0f);
        const float by = ry + (rowH - bh) * 0.5f;
        dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bw, by + bh), b.bg, bh * 0.5f);
        if (ok) {
            const float t = std::clamp(b.value / b.max, 0.0f, 1.0f);
            if (t > 0.0f)
                dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bw * t, by + bh), b.fg, bh * 0.5f);
        }
        char num[16];
        if (ok) snprintf(num, sizeof(num), "%d", static_cast<int>(std::ceil(fmaxf(0.0f, b.value))));
        else    snprintf(num, sizeof(num), "-");
        TextIn(ImVec2(bx + bw + Scale(12.0f), ry), ImVec2(numW, rowH), num, ts::kValue,
               colors::cText, 2);
        ry += rowH;
    }
    return h;
}

// SKR Enhanced brand mark (launcher .brand-symbol): skewed lime tile with
// three dark bars.
void DrawBrandMark(ImDrawList* dl, float x, float y, float size) {
    const float skew = size * 0.268f;   // tan(15deg)
    const ImVec2 tile[4] = {
        ImVec2(x + skew, y), ImVec2(x + size + skew * 0.5f, y),
        ImVec2(x + size - skew * 0.5f, y + size), ImVec2(x, y + size),
    };
    dl->AddConvexPolyFilled(tile, 4, colors::kBrandB);
    const ImVec2 hi[4] = { tile[0], tile[1], ImVec2(x + size, y + size * 0.5f),
                           ImVec2(x + skew * 0.5f, y + size * 0.5f) };
    dl->AddConvexPolyFilled(hi, 4, colors::kBrandA);
    const float barW = size * 5.0f / 34.0f;
    const float gap = size * 3.0f / 34.0f;
    const float heights[3] = { 17.0f / 34.0f, 25.0f / 34.0f, 12.0f / 34.0f };
    const float total = barW * 3.0f + gap * 2.0f;
    float bx = x + (size + skew * 0.5f - total) * 0.5f;
    const float s2 = size * 0.14f;      // tan(8deg)
    for (float hf : heights) {
        const float bh = size * hf;
        const float by = y + (size - bh) * 0.5f;
        const ImVec2 bar[4] = {
            ImVec2(bx + s2 * hf, by), ImVec2(bx + barW + s2 * hf, by),
            ImVec2(bx + barW, by + bh), ImVec2(bx, by + bh),
        };
        dl->AddConvexPolyFilled(bar, 4, colors::kBrandBar);
        bx += barW + gap;
    }
}

// ═══════════════════════════════════════════════════════════════════════
// MAP — embedded GTA San Andreas map (native/pause_map.cpp)
// ═══════════════════════════════════════════════════════════════════════
// View persists for the session: centre in world units, zoom in screen
// pixels per world unit (fit = whole 6000 x 6000 map in the shorter side).
struct MapView { float cx = 0.0f, cy = 0.0f, zoom = 0.0f; bool dragging = false; };
MapView g_mapView;

// Separate from RenderTabMap: that function uses __try, which forbids
// objects with destructors (C2712).
// Remaining road route: thin navy underlay + lime dashes (map_route_draw.h),
// plus a dotted off-road connector when the waypoint is away from the road.
void DrawMapRoute(ImDrawList* dl, ImVec2 a, ImVec2 b, const MapView& v) {
    namespace mr = skr::native_pause::map_route;
    const skr::gps::RouteSnapshot& route = skr::gps::GetRoute();
    if (route.status != skr::gps::NavStatus::Navigating || route.nodes.size() < 2) return;
    const size_t first = static_cast<size_t>((std::max)(0, route.currentIndex - 1));
    if (first + 2 > route.nodes.size()) return;

    // Route length already behind the visible start, in screen px: the dash
    // pattern stays fixed to the road as progress advances (the map scale is
    // uniform, so world length × zoom = on-screen length).
    float travelled = 0.0f;
    for (size_t i = 1; i <= first; ++i)
        travelled += std::hypot(route.nodes[i].pos.x - route.nodes[i - 1].pos.x,
                                route.nodes[i].pos.y - route.nodes[i - 1].pos.y);

    static std::vector<ImVec2> pts;
    pts.clear();
    for (size_t i = first; i < route.nodes.size(); ++i)
        pts.push_back(map::WorldToScreen(a, b, v.cx, v.cy, v.zoom,
                                         route.nodes[i].pos.x, route.nodes[i].pos.y));

    const ImVec2 roadEnd = map::WorldToScreen(a, b, v.cx, v.cy, v.zoom,
                                              route.roadDestination.x, route.roadDestination.y);
    const ImVec2 wp = map::WorldToScreen(a, b, v.cx, v.cy, v.zoom,
                                         route.destination.x, route.destination.y);
    mr::DrawRouteStrokes(dl, pts.data(), static_cast<int>(pts.size()), travelled * v.zoom, a, b,
                         roadEnd, wp, mr::StyleForScreen(GetScreenHeight()));
}

float RenderTabMap(ImDrawList* dl, ImVec2 contentPos, ImVec2 contentSize) {
    map::Update(g_device);

    // The map uses the whole content region (exactly the view height, so the
    // page never scrolls and the wheel is free for zooming).
    const ImVec2 a(contentPos.x, contentPos.y);
    const ImVec2 b(contentPos.x + contentSize.x, contentPos.y + contentSize.y - Scale(12.0f));
    const float w = b.x - a.x, h = b.y - a.y;
    if (w <= 2.0f || h <= 2.0f) return b.y;
    const ImVec2 mid((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);

    MapView& v = g_mapView;
    const float fit = fmaxf(w, h) / map::kWorldSize;
    const float minZoom = fit, maxZoom = fit * 16.0f;
    if (v.zoom <= 0.0f) { v.zoom = fit; v.cx = 0.0f; v.cy = 0.0f; }
    v.zoom = std::clamp(v.zoom, minZoom, maxZoom);

    float px = 0.0f, py = 0.0f, fx = 0.0f, fy = 1.0f;
    const bool hasPlayer = map::GetPlayerPose(px, py, fx, fy);

    // ── Control strip (top) ────────────────────────────────────────────
    // Layout: [N north up] .............. [−] [+] [CENTER ON PLAYER]
    const float pad   = Scale(16.0f);
    const float bs    = Scale(40.0f);
    const float gap   = Scale(8.0f);
    const float stripH = bs;
    const float centreW = TextW("CENTER ON PLAYER", ts::kButton) + Scale(44.0f);

    // Right-side controls: CENTER ON PLAYER, +, -
    const ImVec2 centrePos(b.x - pad - centreW, a.y + pad);
    const ImVec2 plusPos(centrePos.x - gap - bs, a.y + pad);
    const ImVec2 minusPos(plusPos.x - gap - bs, a.y + pad);

    // Left-side: north-up indicator as a styled chip
    const float northW = TextW("N  NORTH UP", ts::kButton) + Scale(44.0f);
    const ImVec2 northPos(a.x + pad, a.y + pad);
    const ImVec2 northSize(northW, stripH);

    // Control hit-test area (union of all controls)
    const ImVec2 ctrlMin(a.x + pad - gap, a.y + pad - gap);
    const ImVec2 ctrlMax(b.x - pad + gap, a.y + pad + stripH + gap);
    const bool overControls = ImGui::IsMouseHoveringRect(ctrlMin, ctrlMax, true);

    // ── Pan / zoom interaction ─────────────────────────────────────────
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetCursorScreenPos(a);
    ImGui::InvisibleButton("##skrmap", ImVec2(w, h));
    if (ImGui::IsItemActivated()) v.dragging = !overControls;
    if (!ImGui::IsItemActive()) v.dragging = false;
    if (v.dragging && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f)) {
        v.cx -= io.MouseDelta.x / v.zoom;
        v.cy += io.MouseDelta.y / v.zoom;
    }
    auto zoomAbout = [&](ImVec2 screen, float factor) {
        const float wx = v.cx + (screen.x - mid.x) / v.zoom;
        const float wy = v.cy - (screen.y - mid.y) / v.zoom;
        const float nz = std::clamp(v.zoom * factor, minZoom, maxZoom);
        v.cx = wx - (screen.x - mid.x) / nz;
        v.cy = wy + (screen.y - mid.y) / nz;
        v.zoom = nz;
    };
    if (!g_uiBlocked && io.MouseWheel != 0.0f && ImGui::IsItemHovered() && !overControls)
        zoomAbout(io.MousePos, std::pow(1.25f, io.MouseWheel));

    // ── Right-click waypoint placement ─────────────────────────────────
    const bool waypointsOn = skr::gps::flags::WaypointsEnabled();
    if (waypointsOn && !g_uiBlocked && ImGui::IsItemHovered() && !overControls &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        float clickWx, clickWy;
        map::ScreenToWorld(a, b, v.cx, v.cy, v.zoom,
                           io.MousePos.x, io.MousePos.y, clickWx, clickWy);
        // Clamp to world bounds.
        clickWx = std::clamp(clickWx, map::kWorldMin, map::kWorldMax);
        clickWy = std::clamp(clickWy, map::kWorldMin, map::kWorldMax);
        // If there's an existing waypoint close to the click, remove it.
        // Otherwise, set a new waypoint at the clicked location.
        if (map::HasWaypoint()) {
            float existWx, existWy;
            map::GetWaypointPos(existWx, existWy);
            const float dx = clickWx - existWx, dy = clickWy - existWy;
            const float distWorld = std::sqrt(dx * dx + dy * dy);
            const float distPx = distWorld * v.zoom;
            if (distPx < Scale(25.0f))
                map::ClearWaypoint();
            else
                map::SetWaypoint(clickWx, clickWy);
        } else {
            map::SetWaypoint(clickWx, clickWy);
        }
    }

    // Route for a waypoint placed here is computed now, not only after resume
    // (the GPS tick may not run while the game is paused).
    if (waypointsOn) {
        float sx, sy, sz;
        if (map::GetPlayerPosition(sx, sy, sz)) skr::gps::SyncWaypoint(sx, sy, sz);
    }

    auto clampCentre = [&]() {
        const float halfW = w * 0.5f / v.zoom, halfH = h * 0.5f / v.zoom;
        v.cx = (halfW >= map::kWorldMax) ? 0.0f
             : std::clamp(v.cx, map::kWorldMin + halfW, map::kWorldMax - halfW);
        v.cy = (halfH >= map::kWorldMax) ? 0.0f
             : std::clamp(v.cy, map::kWorldMin + halfH, map::kWorldMax - halfH);
    };
    clampCentre();

    // ── Map tiles ──────────────────────────────────────────────────────
    dl->AddRectFilled(a, b, colors::kTile, Scale(6.0f));
    ImGui::PushClipRect(a, b, true);
    map::DrawTiles(dl, a, b, v.cx, v.cy, v.zoom);

    // ── Map blips (tRadarTrace at 0xBA86F0) ────────────────────────────
    // Each blip is 0x28 bytes; 175 entries. We draw active blips that
    // have m_nBlipDisplayFlag != 0 (visible on map).
    __try {
        constexpr uintptr_t kRadarTraces = 0xBA86F0;
        constexpr int kMaxTraces = 175;
        constexpr int kTraceSize = 0x28;

        for (int i = 0; i < kMaxTraces; ++i) {
            const uint8_t* trace = reinterpret_cast<const uint8_t*>(kRadarTraces + i * kTraceSize);
            const uint8_t flags = trace[0x26];   // second bitfield byte
            const uint8_t displayFlag = (flags >> 0) & 0x3;  // m_nBlipDisplayFlag: bits 0-1
            const uint8_t blipType    = (flags >> 2) & 0xF;  // m_nBlipType: bits 2-5
            // blipType 0 = unused slot, displayFlag 0 = not shown
            if (blipType == 0 || displayFlag == 0) continue;

            const float bx = *reinterpret_cast<const float*>(trace + 0x08);  // m_vPosition.x
            const float by = *reinterpret_cast<const float*>(trace + 0x0C);  // m_vPosition.y

            const ImVec2 sp = map::WorldToScreen(a, b, v.cx, v.cy, v.zoom, bx, by);
            // Cull if off-screen (with generous margin)
            if (sp.x < a.x - 20 || sp.x > b.x + 20 || sp.y < a.y - 20 || sp.y > b.y + 20)
                continue;

            // Determine colour: use m_dwColour if non-zero, else use blipType heuristic
            const uint32_t rawCol = *reinterpret_cast<const uint32_t*>(trace + 0x00);
            const uint8_t sprite  = trace[0x22];  // m_nBlipSprite
            ImU32 col;
            if (rawCol != 0) {
                // GTA stores colour as ABGR-ish; extract RGBA
                const uint8_t cr = (rawCol >>  0) & 0xFF;
                const uint8_t cg = (rawCol >>  8) & 0xFF;
                const uint8_t cb = (rawCol >> 16) & 0xFF;
                const uint8_t ca = (rawCol >> 24) & 0xFF;
                col = IM_COL32(cr, cg, cb, ca > 0 ? ca : 255);
            } else {
                // Default: red for mission/contact, green for save, yellow otherwise
                if (sprite == 35) col = colors::kAccent;          // SAVEGAME → green
                else if (sprite == 41) col = IM_COL32(255, 50, 50, 255);  // WAYPOINT → red
                else col = IM_COL32(240, 200, 80, 255);           // default yellow
            }

            const float blipR = Scale(5.0f);
            if (sprite == 0) {
                // No sprite: draw a filled square (contact/coord blip)
                dl->AddRectFilled(
                    ImVec2(sp.x - blipR, sp.y - blipR),
                    ImVec2(sp.x + blipR, sp.y + blipR),
                    col, Scale(2.0f));
                dl->AddRect(
                    ImVec2(sp.x - blipR - 1, sp.y - blipR - 1),
                    ImVec2(sp.x + blipR + 1, sp.y + blipR + 1),
                    IM_COL32(0, 0, 0, 160), Scale(2.0f), 0, 1.0f);
            } else {
                // Sprite blip: draw a filled circle
                dl->AddCircleFilled(sp, blipR + Scale(1.0f), IM_COL32(0, 0, 0, 160));
                dl->AddCircleFilled(sp, blipR, col);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Silently skip blips if memory access faults
    }

    // ── GPS route (remaining part, same colours as the minimap line) ─────
    if (waypointsOn) DrawMapRoute(dl, a, b, v);

    // ── Waypoint destination marker (larger, pulsing) ────────────────────
    if (waypointsOn && map::HasWaypoint()) {
        float wpX, wpY;
        map::GetWaypointPos(wpX, wpY);
        const ImVec2 wp = map::WorldToScreen(a, b, v.cx, v.cy, v.zoom, wpX, wpY);
        if (wp.x >= a.x - 30 && wp.x <= b.x + 30 && wp.y >= a.y - 30 && wp.y <= b.y + 30) {
            const float t = static_cast<float>(GetTickCount() % 1200) / 1200.0f;
            const float pulse = 1.0f + 0.15f * std::sin(t * 6.2831853f);
            const float markerR = Scale(10.0f) * pulse;
            const ImU32 wpRed     = IM_COL32(255, 50, 50, 255);
            const ImU32 wpOutline = IM_COL32(180, 20, 20, 200);
            // Outer ring + filled centre
            dl->AddCircleFilled(wp, markerR + Scale(3.0f), IM_COL32(0, 0, 0, 140));
            dl->AddCircleFilled(wp, markerR, wpRed);
            dl->AddCircle(wp, markerR, wpOutline, 0, 2.0f);
            // Small diamond at centre
            const float d = Scale(3.5f);
            const ImVec2 diamond[4] = {
                ImVec2(wp.x, wp.y - d), ImVec2(wp.x + d, wp.y),
                ImVec2(wp.x, wp.y + d), ImVec2(wp.x - d, wp.y)
            };
            dl->AddConvexPolyFilled(diamond, 4, IM_COL32(255, 255, 255, 220));
        }
    }

    // ── Zone name labels on the map ────────────────────────────────────
    // Drawn at mid-to-high zoom levels. Uses major zone centre positions.
    if (v.zoom >= fit * 2.5f) {
        struct ZoneLabel { float x, y; const char* name; };
        static const ZoneLabel kZoneLabels[] = {
            {  1100.0f,  -800.0f, "LOS SANTOS" },
            { -1900.0f,   800.0f, "SAN FIERRO" },
            {  1650.0f,  2100.0f, "LAS VENTURAS" },
            { -400.0f,   2100.0f, "BONE COUNTY" },
            {  -500.0f,   -400.0f, "RED COUNTY" },
            {  -500.0f,   1300.0f, "FLINT COUNTY" },
            {   800.0f,   1200.0f, "WHETSTONE" },
        };
        // Use a subtle, dimmed label style to avoid cluttering the map
        const float alpha = std::clamp((v.zoom / fit - 2.5f) / 1.5f, 0.0f, 0.65f);
        if (alpha > 0.01f) {
            cfont::CRGBA labelCol = { 180, 210, 220, static_cast<uint8_t>(alpha * 255) };
            for (const auto& zl : kZoneLabels) {
                const ImVec2 sp = map::WorldToScreen(a, b, v.cx, v.cy, v.zoom, zl.x, zl.y);
                if (sp.x < a.x - 100 || sp.x > b.x + 100 || sp.y < a.y - 30 || sp.y > b.y + 30)
                    continue;
                TextAt(sp.x, sp.y, zl.name, ts::kEyebrow, labelCol, true, 2);
            }
        }
    }

    // ── Player arrow ───────────────────────────────────────────────────
    if (hasPlayer) {
        const ImVec2 p = map::WorldToScreen(a, b, v.cx, v.cy, v.zoom, px, py);
        const ImVec2 d(fx, -fy);
        const ImVec2 n(-d.y, d.x);
        const float sz = Scale(13.0f);
        dl->AddCircleFilled(p, sz * 1.45f, colors::kAccentRing);
        dl->AddCircle(p, sz * 1.45f, colors::kAccent, 0, 1.5f);
        const ImVec2 tip(p.x + d.x * sz, p.y + d.y * sz);
        const ImVec2 l(p.x - d.x * sz * 0.7f + n.x * sz * 0.7f, p.y - d.y * sz * 0.7f + n.y * sz * 0.7f);
        const ImVec2 r(p.x - d.x * sz * 0.7f - n.x * sz * 0.7f, p.y - d.y * sz * 0.7f - n.y * sz * 0.7f);
        const ImVec2 notch(p.x - d.x * sz * 0.3f, p.y - d.y * sz * 0.3f);
        dl->AddTriangleFilled(tip, l, notch, colors::kKnob);
        dl->AddTriangleFilled(tip, notch, r, colors::kKnob);
        const ImVec2 outline[4] = { tip, l, notch, r };
        dl->AddPolyline(outline, 4, colors::kBrandBar, ImDrawFlags_Closed, 1.5f);
    }
    ImGui::PopClipRect();
    dl->AddRect(a, b, colors::kPanelBorder, Scale(6.0f), 0, 1.0f);

    // ── Top controls ───────────────────────────────────────────────────
    // North-up indicator (left): styled chip with compass icon letter
    {
        const float r = Scale(8.0f);
        dl->AddRectFilled(northPos,
                          ImVec2(northPos.x + northSize.x, northPos.y + northSize.y),
                          IM_COL32(18, 30, 40, 220), r);
        dl->AddRect(northPos,
                    ImVec2(northPos.x + northSize.x, northPos.y + northSize.y),
                    colors::kPanelBorder, r, 0, 1.0f);
        TextIn(northPos, northSize, "N  NORTH UP", ts::kButton, colors::cEyebrow, 1, 0);
    }

    // Zoom +/- and CENTER ON PLAYER (right)
    if (StyledButton(dl, minusPos, ImVec2(bs, bs), "-", Btn::Secondary, ts::kButtonL))
        zoomAbout(mid, 1.0f / 1.5f);
    if (StyledButton(dl, plusPos, ImVec2(bs, bs), "+", Btn::Secondary, ts::kButtonL))
        zoomAbout(mid, 1.5f);
    if (StyledButton(dl, centrePos, ImVec2(centreW, bs), "CENTER ON PLAYER",
                     hasPlayer ? Btn::Primary : Btn::Secondary, ts::kButton) && hasPlayer) {
        v.zoom = fmaxf(v.zoom, fit * 4.0f);
        v.cx = px;
        v.cy = py;
    }

    // CLEAR WAYPOINT button (only shown when a waypoint is active)
    const bool hasWp = waypointsOn && map::HasWaypoint();
    if (hasWp) {
        const float clearW = TextW("CLEAR WAYPOINT", ts::kButton) + Scale(44.0f);
        const ImVec2 clearPos(minusPos.x - gap - clearW, a.y + pad);
        if (StyledButton(dl, clearPos, ImVec2(clearW, bs), "CLEAR WAYPOINT",
                         Btn::Secondary, ts::kButton))
            map::ClearWaypoint();
    }

    clampCentre();

    // ── Status chips (bottom-left): loading / error ────────────────────
    const float chipH  = Scale(32.0f);
    const float chipR  = Scale(8.0f);
    const float chipPx = Scale(14.0f);
    auto statusChip = [&](float x, float y, const char* text, cfont::CRGBA color) {
        const float cw = TextW(text, ts::kHint) + chipPx * 2.0f;
        const ImVec2 c0(x, y), c1(x + cw, y + chipH);
        dl->AddRectFilled(c0, c1, IM_COL32(18, 30, 40, 220), chipR);
        dl->AddRect(c0, c1, colors::kPanelBorder, chipR, 0, 1.0f);
        TextIn(c0, ImVec2(cw, chipH), text, ts::kHint, color, 1, 0);
    };
    char status[96];
    float statusY = b.y - pad - chipH;
    if (const char* err = map::Error()) {
        snprintf(status, sizeof(status), "Map unavailable: %s", err);
        statusChip(a.x + pad, statusY, status, colors::cDanger);
        statusY -= chipH + gap;
    } else if (!map::Finished()) {
        snprintf(status, sizeof(status), "Loading tiles  %d / %d", map::LoadedTiles(), map::kTileCount);
        statusChip(a.x + pad, statusY, status, colors::cMuted);
        statusY -= chipH + gap;
    }

    // ── Info panel (bottom-right): zone, coordinates, heading ──────────
    {
        char city[128] = {}, area[128] = {};
        if (hasPlayer) {
            // Get Z from player ped for zone resolution (px/py from GetPlayerPose are 2D)
            float pz = 0.0f;
            __try {
                auto* ped = *reinterpret_cast<uint8_t**>(0xB7CD98);
                if (ped) {
                    auto* matrix = *reinterpret_cast<float**>(ped + 0x14);
                    if (matrix) pz = matrix[14];
                    else        pz = *reinterpret_cast<float*>(ped + 0x4 + 8);
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            skr::game_state::ResolveZones(px, py, pz, city, sizeof(city), area, sizeof(area));
        }

        // Compute heading in degrees from direction vector
        float headingDeg = 0.0f;
        const char* headingDir = "";
        if (hasPlayer) {
            headingDeg = std::atan2(-fx, fy) * 57.2957795f;
            if (headingDeg < 0.0f) headingDeg += 360.0f;
            // Compass direction
            if      (headingDeg >=  337.5f || headingDeg <  22.5f)  headingDir = "N";
            else if (headingDeg >=  22.5f  && headingDeg <  67.5f)  headingDir = "NE";
            else if (headingDeg >=  67.5f  && headingDeg < 112.5f)  headingDir = "E";
            else if (headingDeg >= 112.5f  && headingDeg < 157.5f)  headingDir = "SE";
            else if (headingDeg >= 157.5f  && headingDeg < 202.5f)  headingDir = "S";
            else if (headingDeg >= 202.5f  && headingDeg < 247.5f)  headingDir = "SW";
            else if (headingDeg >= 247.5f  && headingDeg < 292.5f)  headingDir = "W";
            else                                                     headingDir = "NW";
        }

        // Build the info panel
        const float panelW = Scale(280.0f);
        const float panelPad = Scale(16.0f);
        const float rowH = Scale(30.0f);
        const float headH = Scale(38.0f);
        const float labelW = Scale(80.0f);

        // Count rows: zone/area + coords + heading = 3 (or fewer if no player)
        int rows = hasPlayer ? 3 : 1;
        if (hasPlayer && area[0]) rows = 4;  // zone + area + coords + heading
        const float panelH = headH + rowH * rows + panelPad;

        const float panelX = b.x - pad - panelW;
        const float panelY = b.y - pad - panelH;

        const float r = Scale(10.0f);
        dl->AddRectFilled(ImVec2(panelX, panelY),
                          ImVec2(panelX + panelW, panelY + panelH),
                          IM_COL32(18, 30, 40, 225), r);
        dl->AddRect(ImVec2(panelX, panelY),
                    ImVec2(panelX + panelW, panelY + panelH),
                    colors::kPanelBorder, r, 0, 1.0f);

        // Heading
        TextIn(ImVec2(panelX + panelPad, panelY),
               ImVec2(panelW - panelPad * 2, headH),
               "LOCATION", ts::kEyebrow, colors::cEyebrow, 0, 0);
        dl->AddLine(ImVec2(panelX + panelPad, panelY + headH),
                    ImVec2(panelX + panelW - panelPad, panelY + headH),
                    colors::kLine, 1.0f);

        float ry = panelY + headH + Scale(6.0f);
        const float valX = panelX + panelPad + labelW;
        const float valW = panelW - panelPad * 2 - labelW;

        if (!hasPlayer) {
            TextIn(ImVec2(panelX + panelPad, ry), ImVec2(panelW - panelPad * 2, rowH),
                   "Awaiting player data", ts::kHint, colors::cMuted, 0, 0);
        } else {
            // Zone (city)
            TextIn(ImVec2(panelX + panelPad, ry), ImVec2(labelW, rowH),
                   "Zone", ts::kHint, colors::cHint, 0, 0);
            TextIn(ImVec2(valX, ry), ImVec2(valW, rowH),
                   city[0] ? city : "Unknown", ts::kValue, colors::cTitle, 0, 0);
            ry += rowH;

            // Area (if available)
            if (area[0]) {
                TextIn(ImVec2(panelX + panelPad, ry), ImVec2(labelW, rowH),
                       "Area", ts::kHint, colors::cHint, 0, 0);
                TextIn(ImVec2(valX, ry), ImVec2(valW, rowH),
                       area, ts::kValue, colors::cTitle, 0, 0);
                ry += rowH;
            }

            // Coordinates
            char coords[64];
            snprintf(coords, sizeof(coords), "%.0f, %.0f", px, py);
            TextIn(ImVec2(panelX + panelPad, ry), ImVec2(labelW, rowH),
                   "Pos", ts::kHint, colors::cHint, 0, 0);
            TextIn(ImVec2(valX, ry), ImVec2(valW, rowH),
                   coords, ts::kValue, colors::cTitle, 0, 0);
            ry += rowH;

            // Heading
            char hdg[32];
            snprintf(hdg, sizeof(hdg), "%.0f\xC2\xB0 %s", headingDeg, headingDir);
            TextIn(ImVec2(panelX + panelPad, ry), ImVec2(labelW, rowH),
                   "Heading", ts::kHint, colors::cHint, 0, 0);
            TextIn(ImVec2(valX, ry), ImVec2(valW, rowH),
                   hdg, ts::kValue, colors::cAccent, 0, 0);
        }
    }

    // ── Compact legend (bottom-left, above status chips) ───────────────
    {
        const float legPad = Scale(10.0f);
        const float legRowH = Scale(22.0f);
        const float dotR = Scale(4.0f);
        struct LegendEntry { ImU32 color; const char* label; };
        static const LegendEntry kLegend[] = {
            { IM_COL32(168, 238, 101, 255), "You" },
            { IM_COL32(255,  50,  50, 255), "Waypoint" },
            { IM_COL32(240, 200,  80, 255), "Blip" },
        };
        constexpr int legCount = sizeof(kLegend) / sizeof(kLegend[0]);

        const float legW = Scale(waypointsOn ? 170.0f : 110.0f);
        const int hintRows = waypointsOn ? 1 : 0;
        const float legH = legRowH * (legCount + hintRows) + legPad * 2 + (waypointsOn ? Scale(4.0f) : 0.0f);
        const float legX = a.x + pad;
        const float legY = statusY - gap - legH;

        const float r = Scale(8.0f);
        dl->AddRectFilled(ImVec2(legX, legY), ImVec2(legX + legW, legY + legH),
                          IM_COL32(18, 30, 40, 200), r);
        dl->AddRect(ImVec2(legX, legY), ImVec2(legX + legW, legY + legH),
                    colors::kPanelBorder, r, 0, 1.0f);

        float ly = legY + legPad;
        for (int i = 0; i < legCount; ++i) {
            dl->AddCircleFilled(ImVec2(legX + legPad + dotR, ly + legRowH * 0.5f),
                                dotR, kLegend[i].color);
            TextIn(ImVec2(legX + legPad + dotR * 2 + Scale(8.0f), ly),
                   ImVec2(legW - legPad * 2 - dotR * 2 - Scale(8.0f), legRowH),
                   kLegend[i].label, ts::kHint, colors::cMuted, 0, 0);
            ly += legRowH;
        }
        if (waypointsOn) {
            dl->AddLine(ImVec2(legX + legPad, ly), ImVec2(legX + legW - legPad, ly),
                        IM_COL32(60, 80, 100, 120), 1.0f);
            ly += Scale(4.0f);
            TextIn(ImVec2(legX + legPad, ly),
                   ImVec2(legW - legPad * 2, legRowH),
                   "Right-click: waypoint", ts::kHint, colors::cHint, 0, 0);
        }
    }

    return b.y;
}

float RenderTabGame(ImDrawList* dl, ImVec2 contentPos, ImVec2 contentSize) {
    const auto info = samp::GetServerInfo();
    const bool hasSession = info.link != samp::Link::NoSamp &&
                            info.link != samp::Link::NoNetGame;

    // Only values read from SA-MP / the SKR session are shown.
    const char* status = "Not connected";
    cfont::CRGBA statusColor = colors::cDanger;
    switch (info.link) {
        case samp::Link::Connected:   status = "Connected";          statusColor = colors::cAccent; break;
        case samp::Link::WaitConnect: status = "Waiting to connect"; statusColor = colors::cMuted; break;
        case samp::Link::Connecting:  status = "Connecting";         statusColor = colors::cMuted; break;
        case samp::Link::WaitJoin:    status = "Joining";            statusColor = colors::cMuted; break;
        case samp::Link::Restarting:  status = "Server restarting";  statusColor = colors::cMuted; break;
        case samp::Link::Unknown:     status = "Unknown";            statusColor = colors::cMuted; break;
        default: break;
    }
    const char* serverType = "SA-MP / open.mp";
    switch (skr::game_state::GetConnectionMode()) {
        case skr::game_state::ClientConnectionMode::Enhanced:    serverType = "SKR Runtime (enhanced)"; break;
        case skr::game_state::ClientConnectionMode::Negotiating: serverType = "Checking SKR support";   break;
        default: break;
    }

    char hostname[300], address[300], nickname[96];
    ToDisplayText(info.hostname, hostname, sizeof(hostname));
    char rawAddress[280];
    snprintf(rawAddress, sizeof(rawAddress), "%s:%d", info.address, info.port);
    ToDisplayText(rawAddress, address, sizeof(address));
    ToDisplayText(info.nickname, nickname, sizeof(nickname));

    InfoRow rows[4];
    int count = 0;
    if (hasSession) {
        rows[count++] = { "Server",      hostname[0] ? hostname : "Unknown", colors::cText };
        rows[count++] = { "Address",     info.address[0] ? address : "Unknown", colors::cMuted };
        rows[count++] = { "Status",      status, statusColor };
        rows[count++] = { "Server type", serverType, colors::cMuted };
    } else {
        rows[count++] = { "Status", status, statusColor };
    }
    const char* note = hasSession ? nullptr
                                  : "You are not connected to a multiplayer server.";

    // Left: heading + primary actions. Right: SESSION and PLAYER cards.
    const Columns col = SplitColumns(contentPos, contentSize, 0.38f, Scale(380.0f),
                                     Scale(620.0f), Scale(980.0f));
    float ly = contentPos.y + Scale(24.0f);
    ly += PageHeading(col.lx, ly, col.lw, "SKR ENHANCED / SESSION", "Back to the game",
                      "Resume your session.");
    ly += Scale(28.0f);
    const float btnW = fminf(col.lw, Scale(360.0f));
    const float btnH = Scale(56.0f);
    if (StyledButton(dl, ImVec2(col.lx, ly), ImVec2(btnW, btnH), "RESUME",
                     Btn::Primary, ts::kButtonL)) {
        actions::Resume();
    }
    ly += btnH + Scale(14.0f);
    if (StyledButton(dl, ImVec2(col.lx, ly), ImVec2(btnW, btnH),
                     hasSession ? "DISCONNECT" : "RETURN TO LOBBY",
                     hasSession ? Btn::Danger : Btn::Secondary, ts::kButtonL)) {
        OpenModal(Modal::Disconnect);
    }
    ly += btnH;

    float ry = col.wide ? contentPos.y + Scale(24.0f) : ly + Scale(32.0f);
    const float gap = Scale(20.0f);
    const bool sideBySide = col.rw >= Scale(860.0f);
    const float cardW = sideBySide ? (col.rw - gap) * 0.5f : col.rw;
    const float h1 = DrawInfoCard(dl, col.rx, ry, cardW, "SESSION", rows, count, note);
    float h2 = 0.0f;
    if (sideBySide) {
        h2 = DrawVitalsCard(dl, col.rx + cardW + gap, ry, cardW, nickname);
        ry += fmaxf(h1, h2);
    } else {
        ry += h1 + gap;
        ry += DrawVitalsCard(dl, col.rx, ry, cardW, nickname);
    }
    return fmaxf(ly, ry);
}

// ── SETTINGS → ENHANCED (SKR client options; pause_enhanced.cpp) ─────
float GroupHeading(float x, float y, float width, const char* text) {
    TextIn(ImVec2(x + Scale(16.0f), y + Scale(10.0f)), ImVec2(width - Scale(32.0f), Scale(28.0f)),
           text, ts::kEyebrow, colors::cEyebrow, 0, 0);
    return y + Scale(42.0f);
}

float RenderEnhancedRows(ImDrawList* dl, float x, float y, float width) {
    const float rowStep = RowHeight();
    int step = 0;
    bool apply = false;
    char valueText[64]{};
    enhanced::Prefs prefs = enhanced::Get();

    // Interface
    y = GroupHeading(x, y, width, "INTERFACE");
    snprintf(valueText, sizeof(valueText), "%.2fx", prefs.uiScale);
    DrawSettingStepper(dl, x, y, width, "UI scale", valueText, false, step, apply);
    if (step != 0) {
        prefs.uiScale += 0.05f * static_cast<float>(step);
        enhanced::Set(prefs);
    }
    y += rowStep;
    if (DrawSettingToggle(dl, x, y, width, "Menu animations", prefs.animations)) {
        prefs.animations = !prefs.animations;
        enhanced::Set(prefs);
    }
    y += rowStep;

    // HUD & overlay
    y = GroupHeading(x, y, width, "HUD & OVERLAY");
    const bool fpsCounter = enhanced::GetFpsCounter();
    const unsigned fpsCap = enhanced::GetFpsCap();
    if (DrawSettingToggle(dl, x, y, width, "FPS counter", fpsCounter))
        enhanced::SetFps(!fpsCounter, fpsCap);
    y += rowStep;
    {
        int count = 0;
        const unsigned* caps = enhanced::FpsCapChoices(count);
        int idx = 0;
        for (int i = 0; i < count; ++i) if (caps[i] == fpsCap) idx = i;
        if (fpsCap == 0) snprintf(valueText, sizeof(valueText), "Game default");
        else             snprintf(valueText, sizeof(valueText), "%u FPS", fpsCap);
        DrawSettingStepper(dl, x, y, width, "SKR frame cap", valueText, false, step, apply);
        if (step != 0) enhanced::SetFps(fpsCounter, caps[std::clamp(idx + step, 0, count - 1)]);
        y += rowStep;
        TextIn(ImVec2(x + Scale(16.0f), y), ImVec2(width - Scale(32.0f), Scale(36.0f)),
               "Does not override GTA's frame limiter, VSync or SA-MP limits.",
               ts::kHint, colors::cHint, 0);
        y += Scale(40.0f);
    }
    skr::radar_hud::Config hud = skr::radar_hud::GetConfig();
    if (DrawSettingToggle(dl, x, y, width, "Health & armour under radar", hud.vitalsEnabled)) {
        hud.vitalsEnabled = !hud.vitalsEnabled;
        skr::radar_hud::SetConfig(hud);
    }
    y += rowStep;
    if (DrawSettingToggle(dl, x, y, width, "Health & armour numbers", hud.showNumbers)) {
        hud.showNumbers = !hud.showNumbers;
        skr::radar_hud::SetConfig(hud);
    }
    y += rowStep;
    snprintf(valueText, sizeof(valueText), "%.1fx", hud.scale);
    DrawSettingStepper(dl, x, y, width, "Health & armour size", valueText, false, step, apply);
    if (step != 0) {
        hud.scale = std::clamp(std::round((hud.scale + 0.1f * static_cast<float>(step)) * 10.0f) / 10.0f,
                               0.5f, 2.0f);
        skr::radar_hud::SetConfig(hud);
    }
    y += rowStep;

    // Client
    y = GroupHeading(x, y, width, "CLIENT");
    if (DrawSettingToggle(dl, x, y, width, "Show client version", prefs.showVersion)) {
        prefs.showVersion = !prefs.showVersion;
        enhanced::Set(prefs);
    }
    y += rowStep;
    if (DrawSettingToggle(dl, x, y, width, "Pause menu debug log", prefs.debugLog)) {
        prefs.debugLog = !prefs.debugLog;
        enhanced::Set(prefs);
    }
    y += rowStep;
    if (DrawActionRow(dl, x, y, width, "Reset Enhanced settings", "RESET", Btn::Secondary))
        OpenModal(Modal::ResetEnhanced);
    y += rowStep;
    return y;
}

float RenderTabSettings(ImDrawList* dl, ImVec2 contentPos, ImVec2 contentSize) {
    // Launcher settings layout: category column + settings panel. Narrow
    // screens fall back to sub-tabs above the panel.
    static const char* const kNames[]   = { "Display", "Graphics", "Audio", "Controls", "Language", "Enhanced" };
    static const char* const kUpper[]   = { "DISPLAY", "GRAPHICS", "AUDIO", "CONTROLS", "LANGUAGE", "ENHANCED" };
    static const char* const kHints[]   = { "Brightness, HUD, radar, resolution",
                                            "Draw distance, effects, AA",
                                            "Volumes, radio, user tracks",
                                            "Mouse, key bindings",
                                            "Game text language",
                                            "Interface, HUD, FPS, client" };
    static const char* const kNumbers[] = { "01", "02", "03", "04", "05", "06" };
    const bool wide = contentSize.x >= Scale(900.0f);
    const float catW = wide ? std::clamp(contentSize.x * 0.22f, Scale(230.0f), Scale(320.0f)) : 0.0f;
    const float colGap = Scale(18.0f);
    float y = contentPos.y;

    if (wide) {
        TextIn(ImVec2(contentPos.x + Scale(4.0f), y), ImVec2(catW, Scale(30.0f)),
               "GTA SAN ANDREAS", ts::kEyebrow, colors::cEyebrow, 0, 0);
        float cy = y + Scale(36.0f);
        for (int i = 0; i < kSettingsSectionCount; ++i) {
            const auto section = static_cast<SettingsSection>(i);
            if (section == SettingsSection::Enhanced) {
                // Client settings are a separate group from GTA's own options.
                cy += Scale(10.0f);
                TextIn(ImVec2(contentPos.x + Scale(4.0f), cy), ImVec2(catW, Scale(30.0f)),
                       "SKR ENHANCED", ts::kEyebrow, colors::cEyebrow, 0, 0);
                cy += Scale(36.0f);
            }
            if (DrawCategory(dl, ImVec2(contentPos.x, cy), ImVec2(catW, Scale(68.0f)),
                             kNumbers[i], kNames[i], kHints[i], section == g_settingsSection)) {
                g_settingsSection = section;
                g_tabScroll[static_cast<int>(input::PauseTab::Settings)] = 0.0f;
            }
            cy += Scale(68.0f) + Scale(8.0f);
        }
    } else {
        const float subH = Scale(44.0f);
        const float subW = contentSize.x / static_cast<float>(kSettingsSectionCount);
        for (int i = 0; i < kSettingsSectionCount; ++i) {
            const auto section = static_cast<SettingsSection>(i);
            if (DrawSubTab(dl, ImVec2(contentPos.x + i * subW + Scale(2.0f), y),
                           ImVec2(subW - Scale(4.0f), subH), kUpper[i],
                           section == g_settingsSection)) {
                g_settingsSection = section;
                g_tabScroll[static_cast<int>(input::PauseTab::Settings)] = 0.0f;
            }
        }
        y += subH + Scale(14.0f);
    }

    // Panel (background drawn after its height is known, on a lower channel).
    const float panelX = contentPos.x + (wide ? catW + colGap : 0.0f);
    const float panelW = contentSize.x - (wide ? catW + colGap : 0.0f);
    const float panelY = y;
    dl->ChannelsSplit(2);
    dl->ChannelsSetCurrent(1);

    const int sectionIndex = static_cast<int>(g_settingsSection);
    const float inner = Scale(26.0f);
    char index[16];
    snprintf(index, sizeof(index), "%s / 06", kNumbers[sectionIndex]);
    const bool enhancedSection = g_settingsSection == SettingsSection::Enhanced;
    TextIn(ImVec2(panelX + inner, panelY + Scale(16.0f)), ImVec2(panelW - inner * 2.0f, Scale(24.0f)),
           enhancedSection ? "SKR ENHANCED CLIENT SETTINGS" : "GTA SAN ANDREAS SETTINGS",
           ts::kEyebrow, colors::cEyebrow, 0, 0);
    TextIn(ImVec2(panelX + inner, panelY + Scale(40.0f)), ImVec2(panelW - inner * 2.0f, Scale(40.0f)),
           kNames[sectionIndex], ts::kHeading, colors::cText, 0);
    TextIn(ImVec2(panelX + inner, panelY + Scale(40.0f)), ImVec2(panelW - inner * 2.0f, Scale(40.0f)),
           index, ts::kHint, colors::cDim, 2, 0);
    const float headBottom = panelY + Scale(92.0f);
    dl->AddLine(ImVec2(panelX + inner, headBottom), ImVec2(panelX + panelW - inner, headBottom),
                colors::kRowLine, 1.0f);

    const float x = panelX + inner - Scale(8.0f);
    const float width = panelW - (inner - Scale(8.0f)) * 2.0f;
    y = headBottom + Scale(8.0f);

    settings::Snapshot current{};
    if (enhancedSection) {
        y = RenderEnhancedRows(dl, x, y, width);
    } else if (!settings::Read(current)) {
        TextIn(ImVec2(x, y), ImVec2(width, Scale(56.0f)), "GTA settings are currently unavailable",
               ts::kLabel, colors::cDanger, 1);
        y += Scale(60.0f);
    } else {
        const float rowStep = RowHeight();
        char valueText[64]{};
        bool committed = false;
        int step = 0;
        bool apply = false;

        switch (g_settingsSection) {
            case SettingsSection::Display: {
                float brightness = static_cast<float>(current.brightness);
                snprintf(valueText, sizeof(valueText), "%d", current.brightness);
                if (DrawSettingSlider(dl, "##brightness", x, y, width, "Brightness",
                                      brightness, 0.0f, 384.0f, valueText, committed))
                    settings::SetBrightness(static_cast<int>(brightness + 0.5f), false);
                if (committed) settings::SetBrightness(static_cast<int>(brightness + 0.5f), true);
                y += rowStep;

                if (DrawSettingToggle(dl, x, y, width, "Subtitles", current.subtitles))
                    settings::SetSubtitles(!current.subtitles);
                y += rowStep;
                if (DrawSettingToggle(dl, x, y, width, "HUD", current.hud))
                    settings::SetHud(!current.hud);
                y += rowStep;

                // Original GTA display options (Milestone B)
                {
                    static const char* const kRadar[] = { "Map & blips", "Blips only", "Off" };
                    const int mode = std::clamp(current.radarMode, 0, 2);
                    DrawSettingStepper(dl, x, y, width, "Radar mode", kRadar[mode], false, step, apply);
                    if (step != 0) settings::SetRadarMode((mode + (step > 0 ? 1 : 2)) % 3);
                    y += rowStep;
                }
                if (DrawSettingToggle(dl, x, y, width, "Store gallery photos", current.savePhotos))
                    settings::SetSavePhotos(!current.savePhotos);
                y += rowStep;

                settings::VideoMode mode{};
                if (settings::GetSelectedVideoMode(mode)) {
                    snprintf(valueText, sizeof(valueText), "%d x %d x %d%s",
                             mode.width, mode.height, mode.depth,
                             mode.exclusive ? "" : " windowed");
                    DrawSettingStepper(dl, x, y, width, "Resolution", valueText, true, step, apply);
                    if (step != 0) settings::CycleSelectedVideoMode(step);
                    if (apply) settings::RequestSelectedVideoMode();
                } else {
                    const RowGeom g = MakeRow(x, y, width);
                    BeginRow(dl, g, "Resolution");
                    TextIn(ImVec2(g.ctrlX, g.min.y), ImVec2(g.ctrlW, g.max.y - g.min.y),
                           "Unavailable", ts::kValue, colors::cHint, 0);
                }
                y += rowStep;
                if (DrawActionRow(dl, x, y, width, "Restore display defaults", "RESTORE",
                                  Btn::Secondary))
                    OpenModal(Modal::RestoreDisplay);
                y += rowStep;
                break;
            }

            case SettingsSection::Graphics: {
                float lod = current.drawDistance;
                snprintf(valueText, sizeof(valueText), "%.2f", current.drawDistance);
                if (DrawSettingSlider(dl, "##drawdistance", x, y, width, "Draw distance",
                                      lod, 0.925f, 1.8f, valueText, committed))
                    settings::SetDrawDistance(lod, false);
                if (committed) settings::SetDrawDistance(lod, true);
                y += rowStep;
                {
                    static const char* const kFx[] = { "Low", "Medium", "High", "Very high" };
                    const int fx = std::clamp(current.fxQuality, 0, 3);
                    DrawSettingStepper(dl, x, y, width, "Visual FX quality", kFx[fx], false, step, apply);
                    if (step != 0) settings::SetFxQuality(std::clamp(fx + step, 0, 3));
                    y += rowStep;
                }
                if (DrawSettingToggle(dl, x, y, width, "Frame limiter", current.frameLimiter))
                    settings::SetFrameLimiter(!current.frameLimiter);
                y += rowStep;
                if (DrawSettingToggle(dl, x, y, width, "Widescreen", current.widescreen))
                    settings::SetWidescreen(!current.widescreen);
                y += rowStep;
                if (DrawSettingToggle(dl, x, y, width, "Mip mapping", current.mipMapping))
                    settings::SetMipMapping(!current.mipMapping);
                y += rowStep;
                const int selectedAa = settings::GetSelectedAntiAliasing();
                snprintf(valueText, sizeof(valueText), selectedAa <= 1 ? "OFF" : "%dx", selectedAa);
                DrawSettingStepper(dl, x, y, width, "Anti-aliasing", valueText, true, step, apply);
                if (step != 0) settings::CycleSelectedAntiAliasing(step);
                if (apply) settings::RequestSelectedAntiAliasing();
                y += rowStep;
                // GTA keeps display and graphics defaults in one group.
                if (DrawActionRow(dl, x, y, width, "Restore display defaults", "RESTORE",
                                  Btn::Secondary))
                    OpenModal(Modal::RestoreDisplay);
                y += rowStep;
                break;
            }

            case SettingsSection::Audio: {
                float sfx = static_cast<float>(current.sfxVolume);
                snprintf(valueText, sizeof(valueText), "%d / 64", current.sfxVolume);
                if (DrawSettingSlider(dl, "##sfxvolume", x, y, width, "SFX volume",
                                      sfx, 0.0f, 64.0f, valueText, committed))
                    settings::SetSfxVolume(static_cast<int>(sfx + 0.5f), false);
                if (committed) settings::SetSfxVolume(static_cast<int>(sfx + 0.5f), true);
                y += rowStep;
                float radio = static_cast<float>(current.radioVolume);
                snprintf(valueText, sizeof(valueText), "%d / 64", current.radioVolume);
                committed = false;
                if (DrawSettingSlider(dl, "##radiovolume", x, y, width, "Radio volume",
                                      radio, 0.0f, 64.0f, valueText, committed))
                    settings::SetRadioVolume(static_cast<int>(radio + 0.5f), false);
                if (committed) settings::SetRadioVolume(static_cast<int>(radio + 0.5f), true);
                y += rowStep;
                if (DrawSettingToggle(dl, x, y, width, "Radio EQ", current.radioEq))
                    settings::SetRadioEq(!current.radioEq);
                y += rowStep;
                if (DrawSettingToggle(dl, x, y, width, "Auto retune", current.radioAutoSelect))
                    settings::SetRadioAutoSelect(!current.radioAutoSelect);
                y += rowStep;
                {
                    static const char* const kTracks[] = { "Radio", "Random", "Sequential" };
                    const int tm = std::clamp(current.userTrackMode, 0, 2);
                    DrawSettingStepper(dl, x, y, width, "User tracks play mode", kTracks[tm],
                                       false, step, apply);
                    if (step != 0) settings::SetUserTrackMode((tm + (step > 0 ? 1 : 2)) % 3);
                    y += rowStep;
                }
                if (DrawSettingToggle(dl, x, y, width, "Auto-scan user tracks", current.tracksAutoScan))
                    settings::SetTracksAutoScan(!current.tracksAutoScan);
                y += rowStep;
                // Vanilla fallback: radio station preview and track scanning are
                // driven by GTA's own frontend audio loop.
                if (DrawActionRow(dl, x, y, width, "Radio station & track scan", "GTA OPTIONS",
                                  Btn::Secondary))
                    actions::OpenNativeSettings();
                y += rowStep;
                if (DrawActionRow(dl, x, y, width, "Restore audio defaults", "RESTORE",
                                  Btn::Secondary))
                    OpenModal(Modal::RestoreAudio);
                y += rowStep;
                break;
            }

            case SettingsSection::Controls: {
                float sensitivity = current.mouseSensitivity;
                snprintf(valueText, sizeof(valueText), "%.6f", current.mouseSensitivity);
                if (DrawSettingSlider(dl, "##mousesensitivity", x, y, width, "Mouse sensitivity",
                                      sensitivity, 0.0003125f, 0.005f, valueText, committed))
                    settings::SetMouseSensitivity(sensitivity, false);
                if (committed) settings::SetMouseSensitivity(sensitivity, true);
                y += rowStep;
                if (DrawSettingToggle(dl, x, y, width, "Invert mouse Y", current.invertMouseY))
                    settings::SetInvertMouseY(!current.invertMouseY);
                y += rowStep;
                if (DrawSettingToggle(dl, x, y, width, "Mouse steering", current.mouseSteering))
                    settings::SetMouseSteering(!current.mouseSteering);
                y += rowStep;
                if (DrawSettingToggle(dl, x, y, width, "Mouse flying", current.mouseFlying))
                    settings::SetMouseFlying(!current.mouseFlying);
                y += rowStep;

                static int bindingGroup = 0;
                static const char* const kBindingGroups[] = { "On foot", "Vehicle", "Shared" };
                DrawSettingStepper(dl, x, y, width, "Binding group", kBindingGroups[bindingGroup],
                                   false, step, apply);
                if (step != 0)
                    bindingGroup = (bindingGroup + (step > 0 ? 1 : 2)) % 3;
                y += rowStep;

                TextIn(ImVec2(x + Scale(16.0f), y), ImVec2(width - Scale(32.0f), Scale(34.0f)),
                       "1 PRIMARY     2 SECONDARY     M MOUSE", ts::kEyebrow,
                       colors::cEyebrow, 0, 0);
                y += Scale(34.0f);
                const auto category = static_cast<controls::Category>(bindingGroup);
                for (int i = 0; i < controls::ActionCount(); ++i) {
                    const auto& action = controls::ActionAt(i);
                    if (action.category != category) continue;
                    DrawBindingRow(dl, x, y, width, action);
                    y += rowStep;
                }

                TextIn(ImVec2(x + Scale(16.0f), y), ImVec2(width - Scale(32.0f), Scale(42.0f)),
                       controls::IsCapturing() ? controls::CaptureStatus()
                                               : controls::LastOperationStatus(),
                       ts::kHint, controls::IsCapturing() ? colors::cAccent : colors::cHint, 0);
                y += Scale(42.0f);
                if (DrawActionRow(dl, x, y, width, "Restore control defaults", "RESTORE",
                                  Btn::Secondary))
                    OpenModal(Modal::RestoreControls);
                y += rowStep;
                break;
            }

            case SettingsSection::Language: {
                const int selected = settings::GetSelectedLanguage();
                DrawSettingStepper(dl, x, y, width, "Game language", settings::LanguageName(selected),
                                   true, step, apply);
                if (step != 0) settings::CycleSelectedLanguage(step);
                if (apply && selected != current.language) settings::RequestSelectedLanguage();
                y += rowStep;
                char note[128];
                snprintf(note, sizeof(note), "Current: %s   -   only languages installed in GTA's text folder are listed.",
                         settings::LanguageName(current.language));
                TextIn(ImVec2(x + Scale(16.0f), y), ImVec2(width - Scale(32.0f), Scale(44.0f)),
                       note, ts::kHint, colors::cHint, 0);
                y += Scale(44.0f);
                break;
            }

            case SettingsSection::Enhanced:   // drawn by RenderEnhancedRows above
                break;
        }
    }

    // Vanilla GTA options menu, right-aligned in the panel footer (GTA sections).
    float panelBottom = y + Scale(22.0f);
    if (!enhancedSection) {
        const ImVec2 vanillaSize(Scale(260.0f), ControlHeight());
        const ImVec2 vanillaPos(panelX + panelW - inner - vanillaSize.x, y + Scale(22.0f));
        if (StyledButton(dl, vanillaPos, vanillaSize, "VANILLA SETTINGS", Btn::Secondary, ts::kButton))
            actions::OpenNativeSettings();
        panelBottom = vanillaPos.y + vanillaSize.y + Scale(22.0f);
    }

    dl->ChannelsSetCurrent(0);
    DrawPanel(dl, ImVec2(panelX, panelY), ImVec2(panelX + panelW, panelBottom));
    dl->ChannelsMerge();
    return panelBottom;
}

float RenderTabServers(ImDrawList* dl, ImVec2 contentPos, ImVec2 contentSize) {
    // Server browsing lives in the launcher lobby; this page hands off to it
    // through the existing restart-to-lobby exit path (no in-game browser).
    const auto info = samp::GetServerInfo();
    const bool hasSession = info.link != samp::Link::NoSamp &&
                            info.link != samp::Link::NoNetGame;
    const Columns col = SplitColumns(contentPos, contentSize, 0.42f, Scale(400.0f),
                                     Scale(680.0f), Scale(980.0f));
    float ly = contentPos.y + Scale(24.0f);
    ly += PageHeading(col.lx, ly, col.lw, "SKR ENHANCED / SERVER DIRECTORY",
                      "Find your next world");
    TextIn(ImVec2(col.lx, ly), ImVec2(col.lw, Scale(34.0f)),
           "Browse and join servers from the SKR Enhanced lobby.", ts::kBody, colors::cMuted, 0);
    ly += Scale(34.0f);
    TextIn(ImVec2(col.lx, ly), ImVec2(col.lw, Scale(30.0f)),
           "Returning to the lobby ends the current game session.", ts::kHint, colors::cHint, 0);
    ly += Scale(30.0f) + Scale(28.0f);
    const ImVec2 btnSize(fminf(col.lw, Scale(360.0f)), Scale(56.0f));
    if (StyledButton(dl, ImVec2(col.lx, ly), btnSize, "RETURN TO LOBBY",
                     Btn::Secondary, ts::kButtonL)) {
        OpenModal(Modal::Disconnect);
    }
    ly += btnSize.y;

    char hostname[300], address[300];
    ToDisplayText(info.hostname, hostname, sizeof(hostname));
    char rawAddress[280];
    snprintf(rawAddress, sizeof(rawAddress), "%s:%d", info.address, info.port);
    ToDisplayText(rawAddress, address, sizeof(address));
    InfoRow rows[2];
    int count = 0;
    if (hasSession) {
        rows[count++] = { "Server",  hostname[0] ? hostname : "Unknown", colors::cText };
        rows[count++] = { "Address", info.address[0] ? address : "Unknown", colors::cMuted };
    }
    const float ry = col.wide ? contentPos.y + Scale(24.0f) : ly + Scale(32.0f);
    const float cardW = col.wide ? fminf(col.rw, Scale(760.0f)) : col.rw;
    const float h = DrawInfoCard(dl, col.rx, ry, cardW, "CURRENT SERVER", rows, count,
                                 hasSession ? nullptr : "Not connected to a server.");
    return fmaxf(ly, ry + h);
}

#ifdef SKR_CLIENT_VERSION
constexpr const char* kClientVersion = SKR_CLIENT_VERSION;   // CMake PROJECT_VERSION
#else
constexpr const char* kClientVersion = "unknown";
#endif

float RenderTabAbout(ImDrawList* dl, ImVec2 contentPos, ImVec2 contentSize) {
    const Columns col = SplitColumns(contentPos, contentSize, 0.36f, Scale(380.0f),
                                     Scale(600.0f), Scale(980.0f));
    // Identity
    float ly = contentPos.y + Scale(24.0f);
    const float mark = Scale(64.0f);
    DrawBrandMark(dl, col.lx, ly, mark);
    ly += mark + Scale(22.0f);
    char eyebrow[64];
    snprintf(eyebrow, sizeof(eyebrow), "CLIENT  /  V%s", kClientVersion);
    ly += PageHeading(col.lx, ly, col.lw, eyebrow, "SKR Enhanced",
                      "A standalone multiplayer client for GTA San Andreas.");
    TextIn(ImVec2(col.lx, ly), ImVec2(col.lw, Scale(30.0f)),
           "Connects to SA-MP 0.3.7 and open.mp servers.", ts::kHint, colors::cHint, 0);
    ly += Scale(30.0f);
    TextIn(ImVec2(col.lx, ly), ImVec2(col.lw, Scale(30.0f)),
           "User interface built with Dear ImGui.", ts::kHint, colors::cDim, 0);
    ly += Scale(30.0f);

    // Controls + client information
    static const InfoRow kControls[] = {
        { "ESC",    "Resume / close dialog",   colors::cText },
        { "Q / E",  "Previous / next tab",     colors::cText },
        { "ARROWS", "Move / adjust selection", colors::cText },
        { "ENTER",  "Activate selection",      colors::cText },
        { "WHEEL",  "Scroll page",             colors::cText },
    };
    char display[32];
    snprintf(display, sizeof(display), "%.0f x %.0f", g_layout.sw, g_layout.sh);
    char uiScale[16];
    snprintf(uiScale, sizeof(uiScale), "%.2fx", settings::GetUiScale());
    const InfoRow client[] = {
        { "Version",  kClientVersion, colors::cText },
        { "SA-MP",    input::IsSampR5Loaded() ? "0.3.7-R5" : "Not detected", colors::cText },
        { "Renderer", "Direct3D 9", colors::cText },
        { "Display",  display, colors::cText },
        { "UI scale", uiScale, colors::cText },
    };
    const float gap = Scale(20.0f);
    float ry = col.wide ? contentPos.y + Scale(24.0f) : ly + Scale(32.0f);
    const bool sideBySide = col.rw >= Scale(860.0f);
    const float cardW = sideBySide ? (col.rw - gap) * 0.5f : col.rw;
    const float h1 = DrawInfoCard(dl, col.rx, ry, cardW, "CONTROLS", kControls, 5);
    if (sideBySide) {
        const float h2 = DrawInfoCard(dl, col.rx + cardW + gap, ry, cardW, "CLIENT", client, 5);
        ry += fmaxf(h1, h2);
    } else {
        ry += h1 + gap;
        ry += DrawInfoCard(dl, col.rx, ry, cardW, "CLIENT", client, 5);
    }
    return fmaxf(ly, ry);
}

// ═══════════════════════════════════════════════════════════════════════
// Main Render — Two-Pass Architecture
// ═══════════════════════════════════════════════════════════════════════

// ═══════════════════════════════════════════════════════════════════════
// Confirmation Dialog
// ═══════════════════════════════════════════════════════════════════════
// Drawn on the foreground list so it covers the page and its fallback text.
// Default focus is CANCEL; Left/Right/Up/Down move, Enter activates, ESC
// cancels (input::HandleMessage → renderer::HandleBack).
void DrawModal(float sw, float sh) {
    if (g_modal == Modal::None) return;

    bool activate = false;
    for (;;) {
        const input::NavEvent ev = input::PopNavEvent();
        if (ev == input::NavEvent::None) break;
        if (ev == input::NavEvent::Left || ev == input::NavEvent::Up)    g_modalFocus = 0;
        if (ev == input::NavEvent::Right || ev == input::NavEvent::Down) g_modalFocus = 1;
        if (ev == input::NavEvent::Activate) activate = true;
    }

    // CFont text is flushed after all geometry; drop the page's queued text
    // so it cannot print over the dialog.
    if (cfont::g_cfontAvailable) ResetTextQueue();

    const char* eyebrow = "CONFIRMATION";
    const char* title   = "";
    const char* line1   = "";
    const char* line2   = "";
    const char* cancel  = "CANCEL";
    const char* confirm = "";
    bool secondIsDanger = true;   // video keep/revert: second button is neutral
    char countdown[96];
    switch (g_modal) {
        case Modal::ExitGame:
            title = "Exit game";
            line1 = "Close GTA San Andreas and end this session?";
            line2 = "The launcher session ends when the game closes.";
            confirm = "EXIT GAME";
            break;
        case Modal::Disconnect:
            title = "Return to lobby";
            line1 = "Leave the current session and return to the lobby?";
            line2 = "The game restarts into the SKR Enhanced lobby.";
            confirm = "DISCONNECT";
            break;
        case Modal::RestoreDisplay:
            title = "Restore display defaults";
            line1 = "Reset GTA display and graphics settings to their defaults?";
            line2 = "Uses GTA's built-in defaults and saves them.";
            confirm = "RESTORE";
            break;
        case Modal::RestoreAudio:
            title = "Restore audio defaults";
            line1 = "Reset GTA audio settings to their defaults?";
            line2 = "Uses GTA's built-in defaults and saves them.";
            confirm = "RESTORE";
            break;
        case Modal::RestoreControls:
            title = "Restore control defaults";
            line1 = "Reset mouse settings and all key bindings to GTA's defaults?";
            line2 = "Uses GTA's built-in defaults and saves them.";
            confirm = "RESTORE";
            break;
        case Modal::BindingConflict:
            title = "Replace conflicting binding?";
            line1 = controls::ConflictSummary();
            line2 = "Only bindings that conflict in GTA's action contexts will be replaced.";
            confirm = "REPLACE";
            break;
        case Modal::ResetEnhanced:
            title = "Reset Enhanced settings";
            line1 = "Reset SKR Enhanced interface, HUD and FPS settings?";
            line2 = "GTA's own settings are not affected.";
            confirm = "RESET";
            break;
        case Modal::VideoConfirm: {
            int secs = 0;
            settings::VideoConfirmPending(secs);
            eyebrow = "DISPLAY";
            title = "Keep this resolution?";
            snprintf(countdown, sizeof(countdown),
                     "Reverting to the previous resolution in %d s.", secs);
            line1 = countdown;
            line2 = "Choose Keep if the picture looks correct.";
            cancel = "KEEP";
            confirm = "REVERT";
            secondIsDanger = false;
            break;
        }
        default:
            break;
    }

    ImDrawList* fg = ImGui::GetForegroundDrawList();
    fg->AddRectFilled(ImVec2(0, 0), ImVec2(sw, sh), colors::kModalBackdrop);

    const float w = fminf(Scale(560.0f), sw - Scale(40.0f));
    const float h = Scale(270.0f);
    const ImVec2 p0((sw - w) * 0.5f, (sh - h) * 0.5f);
    const ImVec2 p1(p0.x + w, p0.y + h);
    const float pad = Scale(30.0f);
    const float r = Scale(10.0f);
    fg->AddRectFilled(p0, p1, colors::kTile2, r);
    fg->AddRect(p0, p1, colors::kSecondaryLine, r, 0, 1.0f);

    TextIn(ImVec2(p0.x + pad, p0.y + Scale(24.0f)), ImVec2(w - pad * 2.0f, Scale(24.0f)),
           eyebrow, ts::kEyebrow, colors::cEyebrow, 0, 0);
    TextIn(ImVec2(p0.x + pad, p0.y + Scale(50.0f)), ImVec2(w - pad * 2.0f, Scale(46.0f)),
           title, ts::kHeading, colors::cText, 0);
    TextIn(ImVec2(p0.x + pad, p0.y + Scale(104.0f)), ImVec2(w - pad * 2.0f, Scale(32.0f)),
           line1, ts::kBody, colors::cTitle, 0);
    TextIn(ImVec2(p0.x + pad, p0.y + Scale(136.0f)), ImVec2(w - pad * 2.0f, Scale(30.0f)),
           line2, ts::kHint, colors::cHint, 0);

    // Launcher exit pattern: CANCEL is the primary (green) action and has
    // default focus; the destructive action is the red one.
    const ImVec2 btn(fminf(Scale(190.0f), (w - pad * 2.0f - Scale(12.0f)) * 0.5f), Scale(48.0f));
    const float by = p1.y - pad - btn.y;
    const ImVec2 confirmPos(p1.x - pad - btn.x, by);
    const ImVec2 cancelPos(confirmPos.x - Scale(12.0f) - btn.x, by);

    struct DialogBtn { ImVec2 pos; const char* label; ImU32 bg; ImU32 bgHover; ImU32 border;
                       cfont::CRGBA text; };
    const DialogBtn buttons[2] = {
        { cancelPos, cancel, colors::kPrimary, colors::kPrimaryHover, 0, colors::cOnPrimary },
        secondIsDanger
            ? DialogBtn{ confirmPos, confirm, colors::kDangerBg, colors::kDangerBgHov,
                         colors::kDanger, colors::cDanger }
            : DialogBtn{ confirmPos, confirm, colors::kSecondary, colors::kSecondaryHov,
                         colors::kSecondaryLine, colors::cTitle },
    };
    int clicked = -1;
    const float br = Scale(6.0f);
    for (int i = 0; i < 2; ++i) {
        const ImVec2 b0 = buttons[i].pos;
        const ImVec2 b1(b0.x + btn.x, b0.y + btn.y);
        const bool hovered = ImGui::IsMouseHoveringRect(b0, b1, false);
        if (hovered) g_modalFocus = i;
        if (hovered && ImGui::IsMouseClicked(0)) clicked = i;
        const bool focused = g_modalFocus == i;
        fg->AddRectFilled(b0, b1, (hovered || focused) ? buttons[i].bgHover : buttons[i].bg, br);
        if (buttons[i].border) fg->AddRect(b0, b1, buttons[i].border, br, 0, 1.0f);
        if (focused) {
            const float o = Scale(3.0f);
            fg->AddRect(ImVec2(b0.x - o, b0.y - o), ImVec2(b1.x + o, b1.y + o),
                        colors::kAccent, br + o, 0, 2.0f);
        }
        TextIn(b0, btn, buttons[i].label, ts::kButton, buttons[i].text, 1, 0);
    }
    if (activate) clicked = g_modalFocus;

    if (clicked == 0) {
        if (g_modal == Modal::VideoConfirm) settings::ConfirmVideoMode();
        if (g_modal == Modal::BindingConflict) controls::ResolveConflict(false);
        g_modal = Modal::None;
    } else if (clicked == 1) {
        const Modal m = g_modal;
        g_modal = Modal::None;
        switch (m) {
            case Modal::ExitGame:        actions::ExitGame(); break;
            case Modal::Disconnect:      actions::Disconnect(); break;
            case Modal::RestoreDisplay:  settings::RequestDefaults(settings::DefaultsGroup::Display); break;
            case Modal::RestoreAudio:    settings::RequestDefaults(settings::DefaultsGroup::Audio); break;
            case Modal::RestoreControls: controls::RestoreDefaults(); break;
            case Modal::BindingConflict: controls::ResolveConflict(true); break;
            case Modal::ResetEnhanced:   enhanced::ResetAll(); break;
            case Modal::VideoConfirm:    settings::RevertVideoMode(); break;
            default: break;
        }
    }
}

void RenderPauseMenu() {
    // ── Compute layout from current D3D9 viewport ──────────────────
    ComputeLayout();
    const LayoutInfo& L = g_layout;

    // A dialog owns input; the page below is drawn but inert.
    // The resolution keep/revert prompt follows the settings bridge state.
    {
        int secs = 0;
        const bool pending = settings::VideoConfirmPending(secs);
        if (pending && g_modal == Modal::None) OpenModal(Modal::VideoConfirm);
        if (!pending && g_modal == Modal::VideoConfirm) g_modal = Modal::None;
    }
    if (controls::ConflictPending() && g_modal == Modal::None)
        OpenModal(Modal::BindingConflict);
    if (!controls::ConflictPending() && g_modal == Modal::BindingConflict)
        g_modal = Modal::None;
    g_uiBlocked = g_modal != Modal::None;

    float sw = L.sw;
    float sh = L.sh;

    // ── Fade-in animation (300ms) ───────────────────────────────────
    DWORD now = GetTickCount();
    float elapsed = static_cast<float>(now - g_openTime) / 300.0f;
    g_menuAlpha = (elapsed > 1.0f || !enhanced::Get().animations) ? 1.0f : elapsed;

    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, g_menuAlpha);
    g_textAlpha = g_menuAlpha;
    ResetTextQueue();

    // ── Background: launcher body (#070b11 + top radial #17364b + faint
    // grid). Opaque from the first frame so the game scene never shows.
    ImDrawList* bgDraw = ImGui::GetBackgroundDrawList();
    bgDraw->AddRectFilled(ImVec2(0, 0), ImVec2(sw, sh), colors::kBg);
    bgDraw->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(sw, sh * 0.55f),
                                    colors::kBgGlow, colors::kBgGlow,
                                    IM_COL32(23, 54, 75, 0), IM_COL32(23, 54, 75, 0));
    {
        const float cell = fmaxf(Scale(76.0f), 24.0f);
        const float gridH = sh * 0.6f;
        for (float gx = 0.0f; gx < sw; gx += cell)
            bgDraw->AddLine(ImVec2(gx, 0.0f), ImVec2(gx, gridH), colors::kGrid, 1.0f);
        for (float gy = 0.0f; gy < gridH; gy += cell)
            bgDraw->AddLine(ImVec2(0.0f, gy), ImVec2(sw, gy), colors::kGrid, 1.0f);
    }

    // ── Full-width shell (launcher .shell padding) ──────────────────
    float colX = L.contentX;
    float colW = L.contentW;

    // ── Invisible ImGui window for interaction (covers full viewport)
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(sw, sh));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoBackground |
                             ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::Begin("##SKRPauseMenu", nullptr, flags);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // ── Header (launcher .header): brand mark, pill navigation, and on
    // the right the real connection state + player chip.
    const float headerH = Scale(88.0f);
    dl->AddLine(ImVec2(0.0f, headerH), ImVec2(sw, headerH), colors::kHeaderLine, 1.0f);
    const float markSize = Scale(36.0f);
    DrawBrandMark(dl, colX, (headerH - markSize) * 0.5f, markSize);

    const float pillH = Scale(42.0f);
    const float pillY = (headerH - pillH) * 0.5f;
    const float pillPad = Scale(18.0f);
    float px = colX + markSize + Scale(30.0f);
    auto activeTab = input::GetActiveTab();
    for (int i = 0; i < kTabCount; ++i) {
        const float pw = TextW(kTabNames[i], ts::kNav) + pillPad * 2.0f;
        const ImVec2 tabMin(px, pillY);
        const ImVec2 tabMax(px + pw, pillY + pillH);
        const bool isActive  = (i == static_cast<int>(activeTab));
        const bool isHovered = HoverRect(tabMin, tabMax, false);
        const float r = Scale(7.0f);
        if (isActive) {
            dl->AddRectFilled(tabMin, tabMax, colors::kAccentSoft, r);
            dl->AddRect(tabMin, tabMax, colors::kAccentRing, r, 0, 1.0f);
        } else if (isHovered) {
            dl->AddRectFilled(tabMin, tabMax, colors::kHover, r);
        }
        TextIn(tabMin, ImVec2(pw, pillH), kTabNames[i], ts::kNav,
               isActive ? colors::cAccentText : (isHovered ? colors::cText : colors::cMuted), 1, 0);
        if (isHovered && ImGui::IsMouseClicked(0))
            input::SetActiveTab(static_cast<input::PauseTab>(i));
        px += pw + Scale(6.0f);
    }
    const float navEnd = px + Scale(24.0f);

    {
        // Right side: only real data. Items that would overlap the
        // navigation are dropped (narrow screens).
        const auto info = samp::GetServerInfo();
        float rx = colX + colW;
        char nickname[96];
        ToDisplayText(info.nickname, nickname, sizeof(nickname));
        if (nickname[0]) {
            const float chipH = Scale(46.0f);
            const float avatar = Scale(34.0f);
            const float nameW = fminf(TextW(nickname, ts::kValue), Scale(180.0f));
            const float chipW = avatar + Scale(16.0f) + fmaxf(nameW, TextW("PLAYER", ts::kEyebrow)) + Scale(14.0f);
            if (rx - chipW > navEnd) {
                const ImVec2 c0(rx - chipW, (headerH - chipH) * 0.5f);
                const ImVec2 c1(rx, c0.y + chipH);
                dl->AddRectFilled(c0, c1, colors::kTile, Scale(6.0f));
                dl->AddRect(c0, c1, colors::kLine, Scale(6.0f), 0, 1.0f);
                const ImVec2 a0(c0.x + Scale(6.0f), c0.y + (chipH - avatar) * 0.5f);
                dl->AddRectFilled(a0, ImVec2(a0.x + avatar, a0.y + avatar), colors::kAvatar, Scale(4.0f));
                char initial[2] = { nickname[0], '\0' };
                if (initial[0] >= 'a' && initial[0] <= 'z') initial[0] = static_cast<char>(initial[0] - 32);
                TextIn(a0, ImVec2(avatar, avatar), initial, ts::kNav, colors::cAccentText, 1, 0);
                const float tx = a0.x + avatar + Scale(10.0f);
                TextIn(ImVec2(tx, c0.y + Scale(4.0f)), ImVec2(nameW + Scale(40.0f), chipH * 0.42f),
                       "PLAYER", ts::kEyebrow, colors::cDim, 0, 0);
                char shown[96];
                strncpy(shown, nickname, sizeof(shown) - 1);
                shown[sizeof(shown) - 1] = '\0';
                FitText(shown, sizeof(shown), ts::kValue, Scale(180.0f));
                TextIn(ImVec2(tx, c0.y + chipH * 0.42f), ImVec2(nameW + Scale(40.0f), chipH * 0.52f),
                       shown, ts::kValue, colors::cText, 0, 0);
                rx -= chipW + Scale(18.0f);
            }
        }
        const char* mode = "OFFLINE";
        ImU32 dot = colors::kSwitchOffLine;
        if (info.link == samp::Link::Connected) { mode = "CONNECTED"; dot = colors::kAccent; }
        else if (info.link == samp::Link::Connecting || info.link == samp::Link::WaitConnect ||
                 info.link == samp::Link::WaitJoin) { mode = "CONNECTING"; dot = colors::kSwitchOnLine; }
        else if (info.link == samp::Link::Restarting) { mode = "RESTARTING"; dot = colors::kSwitchOnLine; }
        const float modeW = TextW(mode, ts::kEyebrow) + Scale(18.0f);
        if (rx - modeW > navEnd) {
            const float my = headerH * 0.5f;
            dl->AddCircleFilled(ImVec2(rx - modeW + Scale(3.0f), my), Scale(3.5f), dot);
            TextIn(ImVec2(rx - modeW + Scale(12.0f), 0.0f), ImVec2(modeW, headerH), mode,
                   ts::kEyebrow, colors::cEyebrow, 0, 0);
        }
    }

    // ── Content region (full shell width) ────────────────────────────
    float contentY = headerH + Scale(22.0f);
    float footerH  = Scale(56.0f);
    float contentH = (sh - footerH - Scale(14.0f)) - contentY;
    ImVec2 contentPos(colX, contentY);
    ImVec2 contentSize(colW, contentH);

    // ── Clipped, scrollable content view ───────────────────────────
    static input::PauseTab previousTab = input::PauseTab::Game;
    const auto currentTab = input::GetActiveTab();
    if (currentTab == input::PauseTab::Settings && previousTab != currentTab)
        settings::Refresh();
    previousTab = currentTab;

    const int tabIndex = std::clamp(static_cast<int>(currentTab), 0, kTabCount - 1);
    const ImVec2 viewMin(colX, contentY);
    const ImVec2 viewMax(colX + colW, contentY + contentH);
    float& scroll = g_tabScroll[tabIndex];
    float& extent = g_tabExtent[tabIndex];

    ImGuiIO& io = ImGui::GetIO();
    if (io.MouseWheel != 0.0f && HoverRect(viewMin, viewMax, false))
        scroll -= io.MouseWheel * Scale(60.0f);
    scroll = std::clamp(scroll, 0.0f, fmaxf(0.0f, extent - contentH));

    if (!g_uiBlocked) {
        BeginNav(tabIndex);
    } else {
        g_nav.count = 0;
        g_nav.adjust = 0;
        g_nav.activate = false;
        g_nav.focusSeen = false;
    }
    ImGui::BeginDisabled(g_uiBlocked);   // keeps slider InvisibleButtons inert
    ImGui::PushClipRect(viewMin, viewMax, true);
    g_textClipActive = true;
    g_textClipMin = viewMin;
    g_textClipMax = viewMax;

    const ImVec2 scrolledPos(colX, contentY - scroll);
    float bottom = scrolledPos.y;
    switch (currentTab) {
        case input::PauseTab::Map:      bottom = RenderTabMap(dl, scrolledPos, contentSize);      break;
        case input::PauseTab::Game:     bottom = RenderTabGame(dl, scrolledPos, contentSize);     break;
        case input::PauseTab::Settings: bottom = RenderTabSettings(dl, scrolledPos, contentSize); break;
        case input::PauseTab::Servers:  bottom = RenderTabServers(dl, scrolledPos, contentSize);  break;
        case input::PauseTab::About:    bottom = RenderTabAbout(dl, scrolledPos, contentSize);    break;
        default: break;
    }
    extent = fmaxf(0.0f, bottom - scrolledPos.y + Scale(12.0f));

    g_textClipActive = false;
    ImGui::PopClipRect();
    ImGui::EndDisabled();
    if (!g_uiBlocked) EndNav();

    // Keep the keyboard-focused control inside the view (clamped next frame).
    if (g_nav.keyboard && g_nav.focusSeen) {
        const float pad = Scale(12.0f);
        if (g_nav.focusMin.y < viewMin.y + pad)
            scroll -= (viewMin.y + pad) - g_nav.focusMin.y;
        else if (g_nav.focusMax.y > viewMax.y - pad)
            scroll += g_nav.focusMax.y - (viewMax.y - pad);
    }

    // Scroll indicator (only when the tab overflows the view).
    if (extent > contentH + 1.0f) {
        const float barX = colX + colW - Scale(4.0f);
        const float barW = fmaxf(2.0f, Scale(3.0f));
        dl->AddRectFilled(ImVec2(barX, viewMin.y), ImVec2(barX + barW, viewMax.y),
                          colors::kLine);
        const float thumbH = fmaxf(Scale(24.0f), contentH * contentH / extent);
        const float travel = fmaxf(1.0f, extent - contentH);
        const float thumbY = viewMin.y + (contentH - thumbH) *
                             std::clamp(scroll / travel, 0.0f, 1.0f);
        dl->AddRectFilled(ImVec2(barX, thumbY), ImVec2(barX + barW, thumbY + thumbH),
                          colors::kSecondaryLine);
    }

    // ── Footer (launcher footer: top border, kbd chips, muted labels) ─
    float footerY = sh - footerH;
    dl->AddLine(ImVec2(0.0f, footerY), ImVec2(sw, footerY), colors::kHeaderLine, 1.0f);

    // EXIT GAME — right side; opens a confirmation dialog
    const ImVec2 exitSize(Scale(150.0f), Scale(36.0f));
    const ImVec2 exitPos(colX + colW - exitSize.x, footerY + (footerH - exitSize.y) * 0.5f);
    if (StyledButton(dl, exitPos, exitSize, "EXIT GAME", Btn::Danger, ts::kButton, false)) {
        OpenModal(Modal::ExitGame);
    }

    // Key hints — left side; hints that would reach the exit button are dropped.
    static const char* const kHints[][2] = {
        { "ESC",    "Resume" },
        { "Q / E",  "Tabs" },
        { "ARROWS", "Navigate" },
        { "ENTER",  "Select" },
    };
    float hx = colX;
    const float chipH = Scale(26.0f);
    const float chipY = footerY + (footerH - chipH) * 0.5f;
    float limitX = exitPos.x - Scale(24.0f);
    if (enhanced::Get().showVersion) {
        char version[48];
        snprintf(version, sizeof(version), "SKR ENHANCED  V%s", kClientVersion);
        const float vw = TextW(version, ts::kKey);
        TextIn(ImVec2(exitPos.x - Scale(24.0f) - vw, footerY), ImVec2(vw, footerH), version,
               ts::kKey, colors::cDim, 0, 0);
        limitX -= vw + Scale(24.0f);
    }
    for (const auto& hint : kHints) {
        const float chipW = TextW(hint[0], ts::kKey) + Scale(16.0f);
        const float labelW = TextW(hint[1], ts::kHint);
        if (hx + chipW + Scale(8.0f) + labelW > limitX) break;
        dl->AddRect(ImVec2(hx, chipY), ImVec2(hx + chipW, chipY + chipH), colors::kKbdLine,
                    Scale(3.0f), 0, 1.0f);
        TextIn(ImVec2(hx, chipY), ImVec2(chipW, chipH), hint[0], ts::kKey, colors::cKbd, 1, 0);
        hx += chipW + Scale(8.0f);
        TextIn(ImVec2(hx, chipY), ImVec2(labelW + Scale(2.0f), chipH), hint[1], ts::kHint,
               colors::cDim, 0, 0);
        hx += labelW + Scale(26.0f);
    }

    DrawModal(sw, sh);

    ImGui::End();
    ImGui::PopStyleVar(); // Alpha
}

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════════════
// Public API
// ═══════════════════════════════════════════════════════════════════════

bool Initialize(IDirect3DDevice9* device, HWND window) {
    if (g_imguiInitialized) return true;
    if (!device || !window) return false;

    Log("Initialize: setting up ImGui (device=0x%p, window=0x%p)", device, window);

    g_device = device;
    g_window = window;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;  // Don't save ImGui state to disk

    // We use ImGui primarily for layout/shapes, not text.
    // Load default font at a reasonable size for the rare fallback case.
    // Incorporate UI scale so ImGui fallback text also respects user pref.
    float fontScale = (GetScreenHeight() / 1080.0f) * settings::GetUiScale();
    io.Fonts->AddFontDefault();

    // Typography: prefer Inter from SKR\fonts\ next to the DLL, fall back to
    // Windows system fonts. Multiple rasterisation sizes avoid the blurry
    // downscale that happens when a 40 px glyph is drawn at 12-15 px.
    {
        static const ImWchar kRanges[] = { 0x0020, 0x00FF, 0x0100, 0x017F, 0x0218, 0x021B, 0 };

        // Resolve the DLL directory for bundled fonts.
        char dllDir[MAX_PATH] = {};
        {
            HMODULE self = nullptr;
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(&Initialize), &self);
            if (self) {
                DWORD n = GetModuleFileNameA(self, dllDir, MAX_PATH);
                if (n && n < MAX_PATH) {
                    char* s = strrchr(dllDir, '\\');
                    if (s) *(s + 1) = '\0'; else dllDir[0] = '\0';
                }
            }
        }

        char fontDir[MAX_PATH] = {};
        const UINT winLen = GetWindowsDirectoryA(fontDir, MAX_PATH);

        // Load a single font at a given pixel size: try Inter first, then
        // the Windows fallback list.
        auto loadFont = [&](const char* interFile,
                            const char* const* sysFallbacks, int fallbackCount,
                            float px) -> ImFont* {
            ImFontConfig cfg;
            cfg.OversampleH = 2;
            cfg.OversampleV = 1;
            // Try Inter from SKR\fonts\.
            if (dllDir[0]) {
                char path[MAX_PATH] = {};
                snprintf(path, sizeof(path), "%sSKR\\fonts\\%s", dllDir, interFile);
                if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
                    if (ImFont* f = io.Fonts->AddFontFromFileTTF(path, px, &cfg, kRanges)) {
                        Log("Initialize: Inter font %s @ %.0fpx", interFile, px);
                        return f;
                    }
                }
            }
            // Fall back to Windows fonts.
            if (winLen && winLen < MAX_PATH) {
                for (int i = 0; i < fallbackCount; ++i) {
                    char path[MAX_PATH] = {};
                    snprintf(path, sizeof(path), "%s\\Fonts\\%s", fontDir, sysFallbacks[i]);
                    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) continue;
                    if (ImFont* f = io.Fonts->AddFontFromFileTTF(path, px, &cfg, kRanges)) {
                        Log("Initialize: fallback font %s @ %.0fpx", sysFallbacks[i], px);
                        return f;
                    }
                }
            }
            return nullptr;
        };

        static const char* const kSysHeading[] = { "bahnschrift.ttf", "arialbd.ttf" };
        static const char* const kSysBody[]    = { "segoeui.ttf", "arial.ttf" };

        // Large faces (40 px) for pause-menu text and confirm-dialog titles.
        g_fontHeading = loadFont("Inter-SemiBold.ttf", kSysHeading, 2, 40.0f);
        g_fontBody    = loadFont("Inter-Regular.ttf",  kSysBody,    2, 40.0f);

        // Phone and notification faces at display-native sizes.
        const float displayH = static_cast<float>(GetScreenHeight());
        const float phoneU = displayH / 768.0f;
        const float phoneSm = (std::max)(10.0f, std::round(12.0f * phoneU));
        const float phoneMd = (std::max)(16.0f, std::round(22.0f * phoneU));

        skr::phone::Fonts pf = {};
        pf.heading[0] = loadFont("Inter-SemiBold.ttf", kSysHeading, 2, phoneSm);
        pf.heading[1] = loadFont("Inter-SemiBold.ttf", kSysHeading, 2, phoneMd);
        pf.heading[2] = g_fontHeading;
        pf.body[0]    = loadFont("Inter-Regular.ttf",  kSysBody,    2, phoneSm);
        pf.body[1]    = loadFont("Inter-Regular.ttf",  kSysBody,    2, phoneMd);
        pf.body[2]    = g_fontBody;
        skr::phone::input::SetFonts(pf);
    }
    io.FontGlobalScale = fmaxf(fontScale, 0.75f);

    ApplyGtaStyle();

    if (!ImGui_ImplWin32_Init(window)) {
        Log("Initialize: ImGui_ImplWin32_Init failed");
        ImGui::DestroyContext();
        return false;
    }

    if (!ImGui_ImplDX9_Init(device)) {
        Log("Initialize: ImGui_ImplDX9_Init failed");
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        return false;
    }

    // Check CFont availability
    cfont::g_cfontAvailable = cfont::VerifyCFont();
    Log("Initialize: CFont available = %d", cfont::g_cfontAvailable);

    g_imguiInitialized = true;
    g_deviceObjectsValid = true;
    g_openTime = GetTickCount();
    Log("Initialize: ImGui ready, CFont %s (font scale %.2f)",
        cfont::g_cfontAvailable ? "ENABLED" : "FALLBACK",
        fontScale);
    return true;
}

void Shutdown() {
    if (!g_imguiInitialized) return;

    Log("Shutdown: cleaning up ImGui");
    map::Shutdown();
    skr::phone::wallpaper::Release();

    ImGui_ImplDX9_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    g_imguiInitialized = false;
    g_deviceObjectsValid = false;
    g_fontHeading = nullptr;
    g_fontBody = nullptr;
    g_device = nullptr;
    g_window = nullptr;
}

void Render(IDirect3DDevice9* device, bool drawPauseMenu) {
    if (!g_imguiInitialized || !device) return;

    // SA-MP can replace the live D3D9 device during world handoff. The ImGui
    // backend owns a device reference, so merely replacing g_device leaves it
    // issuing resources against the stale device.
    if (g_device != device) {
        map::OnDeviceChanged();   // map textures belong to the old device
        skr::phone::wallpaper::Release();   // so is the phone wallpaper (reloaded lazily)
        ImGui_ImplDX9_Shutdown();
        g_deviceObjectsValid = ImGui_ImplDX9_Init(device);
        g_device = device;
        // The verified GTA CFont entry points faulted once after SA-MP swapped
        // in its proxy device. Keep the menu operational on the ImGui font
        // path instead of probing CFont again on the replacement device.
        cfont::g_cfontAvailable = false;
        Log("Render: D3D9 device changed, backend re-bound (ok=%d)",
            g_deviceObjectsValid ? 1 : 0);
    }
    if (!g_deviceObjectsValid) return;

    // Reset open time on first frame of a new pause session
    static bool s_wasOpen = false;
    bool isOpen = drawPauseMenu && (input::GetState() == input::PauseState::PauseOpen);
    if (isOpen && !s_wasOpen) {
        g_openTime = GetTickCount();
        g_menuAlpha = 0.0f;
        g_modal = Modal::None;
    }
    s_wasOpen = isOpen;

    // ── Save D3D9 state ─────────────────────────────────────────────
    IDirect3DStateBlock9* stateBlock = nullptr;
    device->CreateStateBlock(D3DSBT_ALL, &stateBlock);

    // ── Pass 1: ImGui frame (backgrounds, shapes, interaction) ──────
    // Gameplay overlay: the Win32 backend must not touch the OS cursor
    // (it updates the cursor inside NewFrame), so the flag is set first.
    ImGuiIO& frameIo = ImGui::GetIO();
    if (!drawPauseMenu) frameIo.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    // The input owner exposes the real Win32 cursor. Keep ImGui's software
    // cursor disabled so SA-MP/CEF cursor hooks cannot produce a double cursor.
    ImGui::GetIO().MouseDrawCursor = false;

    if (drawPauseMenu) {
        // Render the pause menu layout (queues CFont text entries)
        RenderPauseMenu();
    } else {
        // Gameplay overlay: draw-list commands only, no windows, so ImGui
        // never wants input; the OS cursor is left exactly as GTA set it.
        skr::gps::panel::DrawImGui(g_fontHeading, g_fontBody, settings::GetUiScale());

        // Phone overlay: same draw-list-only approach.
        if (skr::phone::input::WantsRender()) {
            skr::phone::wallpaper::Ensure(device);   // once per device; MANAGED pool survives Reset
            auto& ctrl = skr::phone::input::GetController();
            auto& model = skr::phone::input::GetModel();
            auto& views = skr::phone::input::GetViews();
            const auto& fonts = skr::phone::input::GetFonts();

            const ImVec2 ds = ImGui::GetIO().DisplaySize;
            skr::phone::LayoutInput li;
            li.screenW = ds.x;
            li.screenH = ds.y;
            li.uiScale = model.settings.uiScale;
            li.slide = ctrl.Slide();
            skr::phone::Layout lay = skr::phone::ComputeLayout(li);
            skr::phone::FrameHits hits = {};

            if (lay.fits) {
                skr::phone::Pointer ptr = {};
                POINT pt;
                if (g_window && GetCursorPos(&pt)) {
                    ScreenToClient(g_window, &pt);
                    ptr.x = static_cast<float>(pt.x);
                    ptr.y = static_cast<float>(pt.y);
                }
                ptr.down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;

                uint64_t now = GetTickCount64();
                ImDrawList* dl = ImGui::GetBackgroundDrawList();
                skr::phone::Draw(dl, lay, ctrl, model, views, fonts, ptr, now, &hits);
            }
            skr::phone::input::SetFrameResult(lay, hits);
        }

        // Native API notifications (ImGui draw-list, bottom-right, phone-aware).
        // Use the phone's mid-size font faces (~22 px rasterised at 768p) so
        // 14-15 px notification text is a mild downscale, not a 3x reduction
        // from the 40 px pause-menu font that makes glyphs blurry.
        {
            const ImVec2 ds = ImGui::GetIO().DisplaySize;
            const auto& pf = skr::phone::input::GetFonts();
            skr::native_api::ui::DrawContext nc{};
            nc.dl = ImGui::GetBackgroundDrawList();
            nc.fontHeading = pf.heading[1] ? pf.heading[1] : g_fontHeading;
            nc.fontBody    = pf.body[1]    ? pf.body[1]    : g_fontBody;
            nc.fontLarge   = g_fontHeading;
            nc.displayW = ds.x;
            nc.displayH = ds.y;
            nc.phoneVisible = skr::phone::input::WantsRender();
            if (nc.phoneVisible) {
                const auto& pl = skr::phone::input::GetLayout();
                nc.phoneLeft = pl.body.x0;
                nc.phoneTop = pl.body.y0;
            }
            skr::native_api::ui::DrawImGui(nc);
        }
    }

    // End ImGui frame and render geometry to screen
    ImGui::EndFrame();
    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
    if (!drawPauseMenu) frameIo.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;

    // ── Pass 2: CFont text (rendered on top of ImGui geometry) ──────
    if (drawPauseMenu) FlushTextQueue();

    // ── Restore D3D9 state ──────────────────────────────────────────
    if (stateBlock) {
        stateBlock->Apply();
        stateBlock->Release();
    }
}

void OnBeforeReset() {
    map::OnBeforeReset();         // DEFAULT-pool map textures only (MANAGED survive)
    if (!g_imguiInitialized || !g_deviceObjectsValid) return;
    ImGui_ImplDX9_InvalidateDeviceObjects();
    g_deviceObjectsValid = false;
}

void OnAfterReset(IDirect3DDevice9* device) {
    if (!g_imguiInitialized) return;
    if (g_device != device) {
        map::OnDeviceChanged();
        skr::phone::wallpaper::Release();
        ImGui_ImplDX9_Shutdown();
        g_deviceObjectsValid = ImGui_ImplDX9_Init(device);
        g_device = device;
        cfont::g_cfontAvailable = false;
        Log("OnAfterReset: backend moved to replacement device (ok=%d)",
            g_deviceObjectsValid ? 1 : 0);
        return;
    }
    g_device = device;
    g_deviceObjectsValid = ImGui_ImplDX9_CreateDeviceObjects();
}

bool IsReady() {
    return g_imguiInitialized;
}

bool HandleBack() {
    if (controls::IsCapturing()) {
        controls::CancelCapture();
        return true;
    }
    if (g_modal == Modal::None) return false;
    if (g_modal == Modal::VideoConfirm) settings::RevertVideoMode();
    if (g_modal == Modal::BindingConflict) controls::ResolveConflict(false);
    g_modal = Modal::None;
    return true;
}

bool IsModalOpen() {
    return g_modal != Modal::None || controls::IsCapturing();
}

} // namespace skr::native_pause::renderer
