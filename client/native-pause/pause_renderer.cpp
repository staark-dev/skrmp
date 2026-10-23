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
#include "native-pause/pause_input.h"
#include "native-pause/pause_actions.h"
#include "native-pause/pause_settings.h"

#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx9.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#include <cstdint>
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

constexpr int kMaxTextEntries = 128;
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

    float sw = ImGui::GetIO().DisplaySize.x;
    float sh = ImGui::GetIO().DisplaySize.y;

    if (cfont::g_cfontAvailable && g_textQueueCount < kMaxTextEntries) {
        TextEntry& e = g_textQueue[g_textQueueCount++];
        e.x      = ScreenToVirtualX(screenX, sw);
        e.y      = ScreenToVirtualY(screenY, sh);
        e.font   = font;
        e.scaleW = scaleW;
        e.scaleH = scaleH;
        e.color  = color;
        e.centre = centre;
        e.shadow = shadow;
        e.wrapX  = wrapVirtualX;
        strncpy(e.text, text, sizeof(e.text) - 1);
        e.text[sizeof(e.text) - 1] = '\0';
    } else {
        // Fallback: draw with ImGui's draw list (during the active frame)
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        if (dl) {
            ImU32 imCol = IM_COL32(color.r, color.g, color.b, color.a);
            dl->AddText(ImVec2(screenX, screenY), imCol, text);
        }
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

// ═══════════════════════════════════════════════════════════════════════
// SA-MP Data Access (for GAME tab server info)
// ═══════════════════════════════════════════════════════════════════════
namespace samp {

struct ServerInfo {
    bool  connected;
    char  hostname[256];
    int   playerCount;
    int   maxPlayers;
};

ServerInfo GetServerInfo() {
    ServerInfo info{};
    info.connected = false;
    strcpy(info.hostname, "Unknown");
    info.playerCount = 0;
    info.maxPlayers = 0;

    __try {
        HMODULE sampDll = GetModuleHandleA("samp.dll");
        if (!sampDll) return info;

        auto base = reinterpret_cast<uintptr_t>(sampDll);

        // SA-MP 0.3.7-R5: CNetGame* at samp.dll + 0x26EB94
        auto* pNetGame = *reinterpret_cast<uint8_t**>(base + 0x26EB94);
        if (!pNetGame) return info;

        // Game state at CNetGame + 0x3CD (5 = connected)
        uint8_t gameState = *reinterpret_cast<uint8_t*>(pNetGame + 0x3CD);
        info.connected = (gameState == 5);

        // Server hostname at CNetGame + 0x1022 (null-terminated)
        const char* hostname = reinterpret_cast<const char*>(pNetGame + 0x1022);
        strncpy(info.hostname, hostname, sizeof(info.hostname) - 1);
        info.hostname[sizeof(info.hostname) - 1] = '\0';

        // Player pool at CNetGame + 0x3DE (CPlayerPool*)
        auto* pPlayerPool = *reinterpret_cast<uint8_t**>(pNetGame + 0x3DE);
        if (pPlayerPool) {
            // Max players at CNetGame + 0x3C8
            info.maxPlayers = *reinterpret_cast<int32_t*>(pNetGame + 0x3C8);
            // Count connected players from the pool's slot flags
            // For simplicity, use the scoreboard count at pool + 0x0
            // This is approximate; exact offset varies by R5 build
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // SA-MP not available or wrong version — use defaults
    }

    return info;
}

} // namespace samp

// ═══════════════════════════════════════════════════════════════════════
// GTA-Style Color Palette
// ═══════════════════════════════════════════════════════════════════════
namespace colors {
    // Background / overlay
    constexpr ImU32 kOverlay         = IM_COL32(0, 0, 0, 255); // Fully opaque — hides game scene
    constexpr ImU32 kPanelBg         = IM_COL32(12, 12, 16, 235);
    constexpr ImU32 kPanelBorder     = IM_COL32(60, 55, 40, 160);

    // Tab bar
    constexpr ImU32 kTabActive       = IM_COL32(200, 150, 50, 255);
    constexpr ImU32 kTabInactive     = IM_COL32(35, 35, 42, 200);
    constexpr ImU32 kTabHover        = IM_COL32(50, 48, 40, 220);
    constexpr ImU32 kTabBorder       = IM_COL32(80, 70, 40, 120);

    // Text (ImGui fallback colors — CFont uses its own CRGBA)
    constexpr ImU32 kTextPrimary     = IM_COL32(255, 255, 255, 255);
    constexpr ImU32 kTextSecondary   = IM_COL32(170, 170, 180, 200);
    constexpr ImU32 kTextAccent      = IM_COL32(220, 170, 50, 255);
    constexpr ImU32 kTextDimmed      = IM_COL32(100, 100, 110, 180);

    // Buttons
    constexpr ImU32 kButtonBg        = IM_COL32(35, 35, 42, 200);
    constexpr ImU32 kButtonHover     = IM_COL32(55, 52, 45, 230);
    constexpr ImU32 kButtonActive    = IM_COL32(200, 150, 50, 200);
    constexpr ImU32 kButtonGreen     = IM_COL32(40, 120, 50, 200);
    constexpr ImU32 kButtonGreenHov  = IM_COL32(50, 150, 60, 230);
    constexpr ImU32 kButtonRed       = IM_COL32(140, 35, 35, 200);
    constexpr ImU32 kButtonRedHov    = IM_COL32(180, 45, 45, 230);

    // Separators / accents
    constexpr ImU32 kSeparator       = IM_COL32(80, 70, 40, 100);
    constexpr ImU32 kGoldLine        = IM_COL32(200, 150, 50, 200);

    // CFont CRGBA equivalents
    constexpr cfont::CRGBA cWhite      = {255, 255, 255, 255};
    constexpr cfont::CRGBA cGold       = {220, 170, 50, 255};
    constexpr cfont::CRGBA cDimWhite   = {180, 180, 190, 220};
    constexpr cfont::CRGBA cDimGray    = {120, 120, 130, 180};
    constexpr cfont::CRGBA cBlack      = {0, 0, 0, 255};
    constexpr cfont::CRGBA cGreen      = {100, 220, 100, 255};
    constexpr cfont::CRGBA cRed        = {220, 80, 80, 255};
    constexpr cfont::CRGBA cLightGold  = {240, 200, 100, 255};
}

// ═══════════════════════════════════════════════════════════════════════
// State
// ═══════════════════════════════════════════════════════════════════════
bool g_imguiInitialized = false;
IDirect3DDevice9* g_device = nullptr;
HWND g_window = nullptr;
float g_menuAlpha = 0.0f;
DWORD g_openTime = 0;

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
float GetScreenWidth() {
    D3DVIEWPORT9 vp;
    if (g_device && SUCCEEDED(g_device->GetViewport(&vp)))
        return static_cast<float>(vp.Width);
    return 800.0f;
}

float GetScreenHeight() {
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

    // Content width: clamp to a max of ~1200 scaled px for ultrawide,
    // but never wider than 92% of the viewport.
    L.contentMaxW = fminf(L.sw * 0.92f, L.S(1200.0f));
    L.contentW    = L.contentMaxW;
    L.contentX    = (L.sw - L.contentW) * 0.5f;

    // Padding
    L.padX = L.S(30.0f);
    L.padY = L.S(10.0f);

    // Font factor: user UI scale, with a floor so text stays readable
    // at very low resolutions (e.g. 640×480).
    // At 480p dpiScale ≈ 0.44; floor ensures fontFactor ≥ 0.60.
    L.fontFactor = fmaxf(L.uiScale, 0.60f / fmaxf(L.dpiScale, 0.01f));
    // But also cap it so we don't blow up at huge scales
    L.fontFactor = fminf(L.fontFactor, 2.0f);
}

// Scale relative to 1080p reference, incorporating user UI scale
float Scale(float base) {
    return base * g_layout.scale;
}

// ═══════════════════════════════════════════════════════════════════════
// GTA-Style Button (ImGui rect for bg + hit testing, CFont for text)
// ═══════════════════════════════════════════════════════════════════════
// Returns true if clicked.
bool DrawGtaButton(ImDrawList* dl, ImVec2 pos, ImVec2 size, const char* label,
                   ImU32 bgNormal, ImU32 bgHover, uint8_t font = cfont::FONT_MENU,
                   float fontScaleW = 0.45f, float fontScaleH = 0.90f,
                   cfont::CRGBA textColor = colors::cWhite) {

    ImVec2 mousePos = ImGui::GetIO().MousePos;
    bool hovered = (mousePos.x >= pos.x && mousePos.x <= pos.x + size.x &&
                    mousePos.y >= pos.y && mousePos.y <= pos.y + size.y);
    bool clicked = hovered && ImGui::IsMouseClicked(0);

    ImU32 bg = hovered ? bgHover : bgNormal;
    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), bg);

    // Gold border on hover
    if (hovered) {
        dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                    colors::kGoldLine, 0.0f, 0, 1.0f);
    }

    // Queue centred text (apply font factor for UI scale)
    float ff = g_layout.fontFactor;
    float textX = pos.x + size.x * 0.5f;
    float textY = pos.y + size.y * 0.25f; // offset above centre for CFont baseline
    QueueText(textX, textY, label, font, fontScaleW * ff, fontScaleH * ff, textColor, true);

    return clicked;
}

// ═══════════════════════════════════════════════════════════════════════
// Info Row (label : value pair for GAME tab)
// ═══════════════════════════════════════════════════════════════════════
void DrawInfoRow(float x, float y, const char* label, const char* value,
                 cfont::CRGBA labelColor = colors::cDimWhite,
                 cfont::CRGBA valueColor = colors::cWhite) {
    float ff = g_layout.fontFactor;
    QueueText(x, y, label, cfont::FONT_SUBTITLES, 0.34f * ff, 0.72f * ff, labelColor);
    QueueText(x + Scale(160.0f), y, value, cfont::FONT_SUBTITLES, 0.34f * ff, 0.72f * ff, valueColor);
}

// ═══════════════════════════════════════════════════════════════════════
// Tab Content Renderers
// ═══════════════════════════════════════════════════════════════════════

void RenderTabMap(ImDrawList* dl, ImVec2 contentPos, ImVec2 contentSize) {
    float cx = contentPos.x + contentSize.x * 0.5f;
    float cy = contentPos.y;
    float ff = g_layout.fontFactor;

    // Description text
    QueueText(cx, cy + Scale(30.0f),
              "Open the full-screen GTA map",
              cfont::FONT_SUBTITLES, 0.38f * ff, 0.80f * ff, colors::cDimWhite, true);

    QueueText(cx, cy + Scale(60.0f),
              "Press ENTER or click the button below",
              cfont::FONT_SUBTITLES, 0.30f * ff, 0.65f * ff, colors::cDimGray, true);

    // Open Map button
    ImVec2 btnSize(Scale(280.0f), Scale(50.0f));
    ImVec2 btnPos(cx - btnSize.x * 0.5f, cy + Scale(110.0f));

    if (DrawGtaButton(dl, btnPos, btnSize, "OPEN MAP",
                      colors::kButtonBg, colors::kButtonHover)) {
        actions::OpenNativeMap();
    }
}

void RenderTabGame(ImDrawList* dl, ImVec2 contentPos, ImVec2 contentSize) {
    float lx = contentPos.x + g_layout.padX; // left margin
    float cy = contentPos.y;
    float ff = g_layout.fontFactor;

    // Section heading
    QueueText(lx, cy + Scale(15.0f),
              "GAME INFO", cfont::FONT_GOTHIC, 0.52f * ff, 1.00f * ff, colors::cGold);

    // Gold accent line under heading
    float lineY = cy + Scale(42.0f);
    dl->AddLine(ImVec2(lx, lineY),
                ImVec2(lx + Scale(140.0f), lineY),
                colors::kGoldLine, 2.0f);

    // Server info from SA-MP
    auto info = samp::GetServerInfo();
    float rowY = cy + Scale(60.0f);
    float rowH = Scale(26.0f);

    DrawInfoRow(lx, rowY, "Server:", info.hostname);
    rowY += rowH;

    DrawInfoRow(lx, rowY, "Status:",
                info.connected ? "Connected" : "Not connected",
                colors::cDimWhite,
                info.connected ? colors::cGreen : colors::cRed);
    rowY += rowH;

    if (info.maxPlayers > 0) {
        char playerStr[64];
        snprintf(playerStr, sizeof(playerStr), "%d / %d", info.playerCount, info.maxPlayers);
        DrawInfoRow(lx, rowY, "Players:", playerStr);
        rowY += rowH;
    }

    // ── Action buttons ──────────────────────────────────────────────
    float cx = contentPos.x + contentSize.x * 0.5f;
    ImVec2 btnSize(Scale(260.0f), Scale(46.0f));
    float btnX = cx - btnSize.x * 0.5f;
    float btnY = rowY + Scale(30.0f);

    // Resume
    if (DrawGtaButton(dl, ImVec2(btnX, btnY), btnSize, "RESUME",
                      colors::kButtonGreen, colors::kButtonGreenHov)) {
        actions::Resume();
    }

    btnY += Scale(60.0f);

    // Disconnect
    if (DrawGtaButton(dl, ImVec2(btnX, btnY), btnSize, "DISCONNECT",
                      colors::kButtonRed, colors::kButtonRedHov)) {
        actions::Disconnect();
    }
}

void RenderTabSettings(ImDrawList* dl, ImVec2 contentPos, ImVec2 contentSize) {
    float cx = contentPos.x + contentSize.x * 0.5f;
    float cy = contentPos.y;
    float ff = g_layout.fontFactor;

    QueueText(cx, cy + Scale(30.0f),
              "Open GTA's native settings menu",
              cfont::FONT_SUBTITLES, 0.38f * ff, 0.80f * ff, colors::cDimWhite, true);

    QueueText(cx, cy + Scale(60.0f),
              "Video, audio, and control options",
              cfont::FONT_SUBTITLES, 0.30f * ff, 0.65f * ff, colors::cDimGray, true);

    ImVec2 btnSize(Scale(280.0f), Scale(50.0f));
    ImVec2 btnPos(cx - btnSize.x * 0.5f, cy + Scale(110.0f));

    if (DrawGtaButton(dl, btnPos, btnSize, "OPEN SETTINGS",
                      colors::kButtonBg, colors::kButtonHover)) {
        actions::OpenNativeSettings();
    }
}

void RenderTabServers(ImDrawList* dl, ImVec2 contentPos, ImVec2 contentSize) {
    float cx = contentPos.x + contentSize.x * 0.5f;
    float cy = contentPos.y;
    float ff = g_layout.fontFactor;

    // Heading
    QueueText(cx, cy + Scale(15.0f),
              "SERVER BROWSER", cfont::FONT_GOTHIC, 0.48f * ff, 0.95f * ff, colors::cGold, true);

    float lineY = cy + Scale(42.0f);
    dl->AddLine(ImVec2(cx - Scale(80.0f), lineY),
                ImVec2(cx + Scale(80.0f), lineY),
                colors::kGoldLine, 2.0f);

    QueueText(cx, cy + Scale(65.0f),
              "Server browser is available from the main lobby.",
              cfont::FONT_SUBTITLES, 0.32f * ff, 0.68f * ff, colors::cDimWhite, true);

    QueueText(cx, cy + Scale(95.0f),
              "Disconnect to return to the lobby.",
              cfont::FONT_SUBTITLES, 0.30f * ff, 0.65f * ff, colors::cDimGray, true);
}

void RenderTabAbout(ImDrawList* dl, ImVec2 contentPos, ImVec2 contentSize) {
    float cx = contentPos.x + contentSize.x * 0.5f;
    float lx = contentPos.x + g_layout.padX;
    float cy = contentPos.y;
    float ff = g_layout.fontFactor;

    // Title in Pricedown — the GTA accent font
    QueueText(cx, cy + Scale(10.0f),
              "SKR ENHANCED", cfont::FONT_PRICEDOWN, 0.80f * ff, 1.60f * ff,
              colors::cGold, true, 2);

    // Version in Pricedown smaller
    QueueText(cx, cy + Scale(55.0f),
              "v0.1.0", cfont::FONT_PRICEDOWN, 0.45f * ff, 0.90f * ff,
              colors::cLightGold, true, 1);

    // Gold line
    float lineY = cy + Scale(82.0f);
    dl->AddLine(ImVec2(cx - Scale(100.0f), lineY),
                ImVec2(cx + Scale(100.0f), lineY),
                colors::kGoldLine, 2.0f);

    // Description
    float textY = cy + Scale(100.0f);
    QueueText(cx, textY,
              "A custom multiplayer platform for GTA San Andreas.",
              cfont::FONT_SUBTITLES, 0.34f * ff, 0.72f * ff, colors::cDimWhite, true);

    textY += Scale(28.0f);
    QueueText(cx, textY,
              "Built with passion for the SA-MP community.",
              cfont::FONT_SUBTITLES, 0.32f * ff, 0.68f * ff, colors::cDimGray, true);

    // Keyboard shortcuts
    textY += Scale(50.0f);
    QueueText(lx, textY,
              "CONTROLS", cfont::FONT_GOTHIC, 0.42f * ff, 0.85f * ff, colors::cGold);

    textY += Scale(30.0f);
    QueueText(lx, textY, "ESC", cfont::FONT_MENU, 0.35f * ff, 0.72f * ff, colors::cLightGold);
    QueueText(lx + Scale(100.0f), textY, "Open / Close pause menu",
              cfont::FONT_SUBTITLES, 0.30f * ff, 0.65f * ff, colors::cDimWhite);

    textY += Scale(22.0f);
    QueueText(lx, textY, "Q / LEFT", cfont::FONT_MENU, 0.35f * ff, 0.72f * ff, colors::cLightGold);
    QueueText(lx + Scale(100.0f), textY, "Previous tab",
              cfont::FONT_SUBTITLES, 0.30f * ff, 0.65f * ff, colors::cDimWhite);

    textY += Scale(22.0f);
    QueueText(lx, textY, "E / RIGHT", cfont::FONT_MENU, 0.35f * ff, 0.72f * ff, colors::cLightGold);
    QueueText(lx + Scale(100.0f), textY, "Next tab",
              cfont::FONT_SUBTITLES, 0.30f * ff, 0.65f * ff, colors::cDimWhite);

    textY += Scale(22.0f);
    QueueText(lx, textY, "ENTER", cfont::FONT_MENU, 0.35f * ff, 0.72f * ff, colors::cLightGold);
    QueueText(lx + Scale(100.0f), textY, "Activate selection",
              cfont::FONT_SUBTITLES, 0.30f * ff, 0.65f * ff, colors::cDimWhite);
}

// ═══════════════════════════════════════════════════════════════════════
// Main Render — Two-Pass Architecture
// ═══════════════════════════════════════════════════════════════════════

void RenderPauseMenu() {
    // ── Compute layout from current D3D9 viewport ──────────────────
    ComputeLayout();
    const LayoutInfo& L = g_layout;
    float sw = L.sw;
    float sh = L.sh;
    float ff = L.fontFactor;

    // ── Fade-in animation (300ms) ───────────────────────────────────
    DWORD now = GetTickCount();
    float elapsed = static_cast<float>(now - g_openTime) / 300.0f;
    g_menuAlpha = (elapsed > 1.0f) ? 1.0f : elapsed;

    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, g_menuAlpha);
    ResetTextQueue();

    // ── Full-screen opaque overlay (hides live game scene) ──────────
    ImDrawList* bgDraw = ImGui::GetBackgroundDrawList();
    uint8_t overlayAlpha = static_cast<uint8_t>(255.0f * g_menuAlpha);
    bgDraw->AddRectFilled(ImVec2(0, 0), ImVec2(sw, sh),
                          IM_COL32(0, 0, 0, overlayAlpha));

    // ── Full-screen panel background ────────────────────────────────
    bgDraw->AddRectFilled(ImVec2(0, 0), ImVec2(sw, sh), colors::kPanelBg);

    // Top gold accent line (full width)
    bgDraw->AddRectFilled(ImVec2(0, 0), ImVec2(sw, 2.0f), colors::kGoldLine);

    // ── Content column (centred, clamped for ultrawide) ─────────────
    float colX = L.contentX;
    float colW = L.contentW;

    // Side borders for content column (visual framing on ultrawide)
    if (L.aspectRatio > 2.0f) {
        bgDraw->AddLine(ImVec2(colX, 0), ImVec2(colX, sh),
                        colors::kPanelBorder, 1.0f);
        bgDraw->AddLine(ImVec2(colX + colW, 0), ImVec2(colX + colW, sh),
                        colors::kPanelBorder, 1.0f);
    }

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

    // ── Title: "SKR ENHANCED" in FONT_GOTHIC ────────────────────────
    float titleX = colX + colW * 0.5f;
    float titleY = Scale(12.0f);
    QueueText(titleX, titleY, "SKR ENHANCED",
              cfont::FONT_GOTHIC, 0.68f * ff, 1.35f * ff, colors::cGold, true, 2);

    // Gold accent line under title
    float accentY = Scale(48.0f);
    dl->AddLine(ImVec2(titleX - Scale(80.0f), accentY),
                ImVec2(titleX + Scale(80.0f), accentY),
                colors::kGoldLine, 2.0f);

    // ── Tab bar (spans content column) ──────────────────────────────
    float tabBarY = Scale(58.0f);
    float tabH    = Scale(34.0f);
    float tabW    = colW / kTabCount;
    auto activeTab = input::GetActiveTab();

    for (int i = 0; i < kTabCount; ++i) {
        ImVec2 tabMin(colX + i * tabW, tabBarY);
        ImVec2 tabMax(tabMin.x + tabW - 1.0f, tabBarY + tabH);
        bool isActive  = (i == static_cast<int>(activeTab));

        ImVec2 mousePos = ImGui::GetIO().MousePos;
        bool isHovered = (mousePos.x >= tabMin.x && mousePos.x <= tabMax.x &&
                          mousePos.y >= tabMin.y && mousePos.y <= tabMax.y);

        // Tab background
        ImU32 tabBg = isActive  ? colors::kTabActive :
                      isHovered ? colors::kTabHover  : colors::kTabInactive;
        dl->AddRectFilled(tabMin, tabMax, tabBg);

        // Active: gold bottom indicator
        if (isActive) {
            dl->AddRectFilled(ImVec2(tabMin.x, tabMax.y - 3.0f), tabMax,
                              colors::kTabActive);
        }

        // Tab label (FONT_MENU)
        cfont::CRGBA tabTextCol = isActive  ? colors::cBlack :
                                  isHovered ? colors::cWhite : colors::cDimWhite;
        float tabCentreX = tabMin.x + (tabW - 1.0f) * 0.5f;
        float tabTextY   = tabBarY + Scale(5.0f);
        QueueText(tabCentreX, tabTextY, kTabNames[i],
                  cfont::FONT_MENU, 0.40f * ff, 0.82f * ff, tabTextCol, true, 0);

        // Click handling
        if (isHovered && ImGui::IsMouseClicked(0)) {
            input::SetActiveTab(static_cast<input::PauseTab>(i));
        }
    }

    // Separator below tabs (content column width)
    float sepY = tabBarY + tabH + 1.0f;
    dl->AddLine(ImVec2(colX, sepY), ImVec2(colX + colW, sepY),
                colors::kSeparator, 1.0f);

    // ── Tab content area (within content column) ────────────────────
    float contentY = sepY + Scale(8.0f);
    float footerH  = Scale(50.0f);
    float contentH = (sh - footerH) - contentY;
    ImVec2 contentPos(colX, contentY);
    ImVec2 contentSize(colW, contentH);

    // Use an ImGui child window for scrollable content overflow
    ImGui::SetCursorScreenPos(contentPos);
    ImGui::BeginChild("##TabContent", contentSize, false,
                      ImGuiWindowFlags_NoBackground);

    switch (input::GetActiveTab()) {
        case input::PauseTab::Map:      RenderTabMap(dl, contentPos, contentSize);      break;
        case input::PauseTab::Game:     RenderTabGame(dl, contentPos, contentSize);     break;
        case input::PauseTab::Settings: RenderTabSettings(dl, contentPos, contentSize); break;
        case input::PauseTab::Servers:  RenderTabServers(dl, contentPos, contentSize);  break;
        case input::PauseTab::About:    RenderTabAbout(dl, contentPos, contentSize);    break;
        default: break;
    }

    ImGui::EndChild();

    // ── Footer (anchored to bottom of viewport) ─────────────────────
    float footerY = sh - footerH;

    // Separator
    dl->AddLine(ImVec2(colX, footerY),
                ImVec2(colX + colW, footerY),
                colors::kSeparator, 1.0f);

    // "ESC to close" — left side (FONT_SUBTITLES, dimmed)
    QueueText(colX + Scale(20.0f), footerY + Scale(12.0f),
              "ESC to close",
              cfont::FONT_SUBTITLES, 0.26f * ff, 0.55f * ff, colors::cDimGray);

    // "Q/E switch tabs" — centre
    QueueText(colX + colW * 0.5f, footerY + Scale(12.0f),
              "Q / E  switch tabs",
              cfont::FONT_SUBTITLES, 0.26f * ff, 0.55f * ff, colors::cDimGray, true);

    // EXIT GAME button — right side
    ImVec2 exitSize(Scale(100.0f), Scale(28.0f));
    ImVec2 exitPos(colX + colW - exitSize.x - Scale(15.0f),
                   footerY + Scale(10.0f));
    if (DrawGtaButton(dl, exitPos, exitSize, "EXIT GAME",
                      colors::kButtonRed, colors::kButtonRedHov,
                      cfont::FONT_MENU, 0.32f, 0.65f, colors::cWhite)) {
        actions::ExitGame();
    }

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
    g_openTime = GetTickCount();
    Log("Initialize: ImGui ready, CFont %s (font scale %.2f)",
        cfont::g_cfontAvailable ? "ENABLED" : "FALLBACK",
        fontScale);
    return true;
}

void Shutdown() {
    if (!g_imguiInitialized) return;

    Log("Shutdown: cleaning up ImGui");

    ImGui_ImplDX9_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    g_imguiInitialized = false;
    g_device = nullptr;
    g_window = nullptr;
}

void Render(IDirect3DDevice9* device) {
    if (!g_imguiInitialized || !device) return;

    // Update device reference (may change after Reset)
    g_device = device;

    // Reset open time on first frame of a new pause session
    static bool s_wasOpen = false;
    bool isOpen = (input::GetState() == input::PauseState::PauseOpen);
    if (isOpen && !s_wasOpen) {
        g_openTime = GetTickCount();
        g_menuAlpha = 0.0f;
    }
    s_wasOpen = isOpen;

    // ── Save D3D9 state ─────────────────────────────────────────────
    IDirect3DStateBlock9* stateBlock = nullptr;
    device->CreateStateBlock(D3DSBT_ALL, &stateBlock);

    // ── Pass 1: ImGui frame (backgrounds, shapes, interaction) ──────
    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    // Show software cursor during pause
    ImGui::GetIO().MouseDrawCursor = true;

    // Render the pause menu layout (queues CFont text entries)
    RenderPauseMenu();

    // End ImGui frame and render geometry to screen
    ImGui::EndFrame();
    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());

    // ── Pass 2: CFont text (rendered on top of ImGui geometry) ──────
    FlushTextQueue();

    // ── Restore D3D9 state ──────────────────────────────────────────
    if (stateBlock) {
        stateBlock->Apply();
        stateBlock->Release();
    }
}

void OnBeforeReset() {
    if (!g_imguiInitialized) return;
    ImGui_ImplDX9_InvalidateDeviceObjects();
}

void OnAfterReset(IDirect3DDevice9* device) {
    if (!g_imguiInitialized) return;
    g_device = device;
    ImGui_ImplDX9_CreateDeviceObjects();
}

bool IsReady() {
    return g_imguiInitialized;
}

// ── TEST_ONLY: ImGui diagnostic probe ───────────────────────────────
// Runs a minimal ImGui frame with a single cyan filled rectangle at the
// top-right corner of the screen.  This verifies that the full ImGui
// pipeline (NewFrame → draw commands → Render → RenderDrawData) produces
// visible pixels.  Called every frame, regardless of pause state.
void RenderDiagnosticProbe(IDirect3DDevice9* device) {
    if (!g_imguiInitialized || !device) return;

    g_device = device;

    // Save D3D9 state
    IDirect3DStateBlock9* stateBlock = nullptr;
    device->CreateStateBlock(D3DSBT_ALL, &stateBlock);

    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    // Draw a 60x40 cyan rectangle near top-right
    float sw = ImGui::GetIO().DisplaySize.x;
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    fg->AddRectFilled(
        ImVec2(sw - 80.0f, 10.0f),
        ImVec2(sw - 20.0f, 50.0f),
        IM_COL32(0, 255, 255, 255));  // Solid cyan

    ImGui::EndFrame();
    ImGui::Render();

    ImDrawData* dd = ImGui::GetDrawData();
    ImGui_ImplDX9_RenderDrawData(dd);

    // Throttled diagnostics about draw data
    static DWORD s_lastLog = 0;
    DWORD now = GetTickCount();
    if (now - s_lastLog >= 2000) {
        s_lastLog = now;
        Log("DIAG PROBE imgui: CmdLists=%d TotalVtx=%d TotalIdx=%d "
            "DisplaySize=%.0fx%.0f",
            dd->CmdListsCount, dd->TotalVtxCount, dd->TotalIdxCount,
            dd->DisplaySize.x, dd->DisplaySize.y);
    }

    // Restore D3D9 state
    if (stateBlock) {
        stateBlock->Apply();
        stateBlock->Release();
    }
}

} // namespace skr::native_pause::renderer
