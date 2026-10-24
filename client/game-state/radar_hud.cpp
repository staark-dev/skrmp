#include "game-state/radar_hud.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <d3d9.h>

namespace skr::radar_hud {
namespace {

// ── GTA SA 1.0 US — provenance: deps/omp-cef/src/client/core/game_sa ──
// CHud.cpp:    DrawPlayerInfo 0x58EAF0, RenderArmorBar 0x5890A0,
//              RenderHealthBar 0x589270 (cdecl int playerId, int x, int y),
//              CHud::Draw 0x58FAE0 (end of the DrawPlayerInfo scan window).
// CSprite2d.cpp: DrawRect(CRect const&, CRGBA const&) 0x727B60 (cdecl).
// CRect.h:     memory order x1, y1, x2, y2.
// CFont.cpp:   SetScale 0x719380, SetColor 0x719430, SetFontStyle 0x719490,
//              SetWrapx 0x7194D0, SetDropColor 0x719510,
//              SetDropShadowPosition 0x719570, SetEdge 0x719590,
//              SetProportional 0x7195B0, SetBackground(bool,bool) 0x7195C0,
//              SetJustify 0x719600, SetOrientation 0x719610
//              (ALIGN_CENTER 0, LEFT 1, RIGHT 2), PrintString 0x71A700;
//              eFontStyle FONT_SUBTITLES = 1. Coordinates are screen pixels.
// CPed.h:      m_fHealth +0x540, m_fMaxHealth +0x544, m_fArmour +0x548.
// CWorld::Players 0xB7CD98 (CPlayerInfo, m_pPed at +0).
// RsGlobal.maximumWidth/Height 0xC17044 / 0xC17048.
// omp-cef ui/hud_manager.cpp hides HEALTH / ARMOUR by writing 0xC3 to the
// RenderHealthBar / RenderArmorBar entry.
constexpr uintptr_t kDrawPlayerInfo   = 0x58EAF0;
constexpr uintptr_t kHudDraw          = 0x58FAE0;
constexpr uintptr_t kRenderArmorBar   = 0x5890A0;
constexpr uintptr_t kRenderHealthBar  = 0x589270;
constexpr uintptr_t kSprite2dDrawRect = 0x727B60;
constexpr uintptr_t kFontSetScale        = 0x719380;
constexpr uintptr_t kFontSetColor        = 0x719430;
constexpr uintptr_t kFontSetStyle        = 0x719490;
constexpr uintptr_t kFontSetWrapx        = 0x7194D0;
constexpr uintptr_t kFontSetDropColor    = 0x719510;
constexpr uintptr_t kFontSetDropShadow   = 0x719570;
constexpr uintptr_t kFontSetEdge         = 0x719590;
constexpr uintptr_t kFontSetProportional = 0x7195B0;
constexpr uintptr_t kFontSetBackground   = 0x7195C0;
constexpr uintptr_t kFontSetJustify      = 0x719600;
constexpr uintptr_t kFontSetOrientation  = 0x719610;
constexpr uintptr_t kFontPrintString     = 0x71A700;
constexpr uintptr_t kPlayers          = 0xB7CD98;
constexpr uintptr_t kPedHealth        = 0x540;
constexpr uintptr_t kPedMaxHealth     = 0x544;
constexpr uintptr_t kPedArmour        = 0x548;
constexpr uintptr_t kRsMaxWidth       = 0xC17044;
constexpr uintptr_t kRsMaxHeight      = 0xC17048;
// Diagnostics only (read, never written):
constexpr uintptr_t kRadarRect        = 0x8D0920;   // CRadar::m_radarRect (CRadar.cpp)
constexpr uintptr_t kRwD3D9Device     = 0xC97C28;   // IDirect3DDevice9* (NATIVE_RADAR_HUD_REFERENCE.md, RE)
constexpr short     kFontSubtitles    = 1;
constexpr int       kAlignLeft        = 1;

constexpr float kHudBaseWidth  = 640.0f;
constexpr float kHudBaseHeight = 448.0f;

// Layout in HUD units (thickness scaled by Config::scale). One horizontal
// strip exactly as wide as the radar rect: health left half, armour right
// half, split by a small gap. Numbers (optional) sit to the right of it.
constexpr float kBarH        = 5.0f;   // ~8-10 px at 768p (sy ≈ 1.71)
constexpr float kBarGap      = 1.5f;   // horizontal health | armour separation
constexpr float kBorderPx    = 2.0f;   // radar frame thickness in screen pixels
constexpr float kNumberGap   = 3.0f;
constexpr float kNumberW     = 14.0f;  // advance per number when both shown
constexpr float kScreenMargin = 4.0f;
// CFont glyph metrics are not documented in the SDK; these are visual
// estimates for FONT_SUBTITLES and need confirmation in-game.
constexpr float kTextScaleX  = 0.14f;
constexpr float kTextScaleY  = 0.28f;
constexpr float kTextUnitsPerScaleY = 16.0f;   // est. glyph height per 1.0 scaleY
// hud.ini layout version. v1 wrote ShowNumbers=1 and Padding as defaults.
constexpr int kConfigVersion = 2;

struct CRect { float x1, y1, x2, y2; };
struct CRGBA { uint8_t r, g, b, a; };

using DrawRectFn  = void(__cdecl*)(const CRect&, const CRGBA&);
using RenderBarFn = void(__cdecl*)(int, int, int);

// Palette — GTA V-style minimal HUD: no panel, no accent line.
// Health = launcher lime #A8EE65; armour = muted blue. Tracks are dark,
// translucent tints of the same hue so an empty bar still reads as a bar.
constexpr CRGBA kDebugRadar = {255,   0, 255, 255 };  // debug overlay: measured radar
constexpr CRGBA kDebugStrip = {  0, 255, 255, 255 };  // debug overlay: strip
constexpr CRGBA kBorder    = { 10,  10,  12, 200 };   // subtle dark frame around map
constexpr CRGBA kHealth    = {168, 238, 101, 255 };   // #A8EE65 (launcher --green)
constexpr CRGBA kHealthBg  = { 42,  60,  26, 170 };
constexpr CRGBA kArmour    = { 92, 140, 182, 255 };   // #5C8CB6
constexpr CRGBA kArmourBg  = { 26,  40,  54, 170 };
constexpr CRGBA kText      = {244, 248, 250, 255 };   // --text #f4f8fa

// ── State ───────────────────────────────────────────────────────────
Config   g_config;
bool     g_configLoaded = false;
FILETIME g_configStamp = {};
DWORD    g_lastConfigCheck = 0;

bool     g_hooksTried = false;
bool     g_hooksOk = false;      // health call site(s) redirected
bool     g_squareMode = false;   // true when radar is Square/Custom (SKR strip active)
unsigned g_hudFrame = 0;
unsigned g_radarFrame = 0xFFFFFFFFu;
unsigned g_vitalsFrame = 0xFFFFFFFFu;
float    g_radarX = 0, g_radarY = 0, g_radarW = 0, g_radarH = 0;
bool     g_radarKnown = false;   // a measured rect exists (this or an earlier frame)
RadarGeometry g_geometry = {};
float    g_stripX = 0, g_stripY = 0, g_stripW = 0, g_stripH = 0;
DWORD    g_lastGeometryLog = 0;
int      g_serverVitalsNumbers = -1;   // SetServerVitalsNumbers; -1 = hud.ini decides

// ── Log (SKR\native_hud.log next to gta_sa.exe) ─────────────────────
void Log(const char* fmt, ...) {
    char path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    char* slash = strrchr(path, '\\');
    if (!slash) return;
    snprintf(slash + 1, MAX_PATH - (slash + 1 - path), "SKR\\native_hud.log");
    FILE* f = fopen(path, "a");
    if (!f) return;
    fprintf(f, "[%lu] ", GetTickCount());
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fprintf(f, "\n");
    fclose(f);
}

// ── Config (%LOCALAPPDATA%\SKR\hud.ini, same scheme as fps.ini) ─────
std::wstring ConfigPath(bool createFolder) {
    PWSTR local = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)) || !local)
        return {};
    std::wstring dir(local);
    CoTaskMemFree(local);
    dir += L"\\SKR";
    if (createFolder && !CreateDirectoryW(dir.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS)
        return {};
    return dir + L"\\hud.ini";
}

float ReadFloat(const wchar_t* section, const wchar_t* key, float fallback,
                float lo, float hi, const std::wstring& path) {
    wchar_t buf[32] = {};
    GetPrivateProfileStringW(section, key, L"", buf, 32, path.c_str());
    if (!buf[0]) return fallback;
    wchar_t* end = nullptr;
    const float v = static_cast<float>(wcstod(buf, &end));
    if (end == buf || !std::isfinite(v)) return fallback;
    return std::clamp(v, lo, hi);
}

Config Clamp(Config c) {
    c.offsetX = std::clamp(c.offsetX, -200.0f, 200.0f);
    c.offsetY = std::clamp(c.offsetY, -200.0f, 200.0f);
    c.scale   = std::clamp(c.scale, 0.5f, 2.0f);
    c.spacing = std::clamp(c.spacing, 0.0f, 20.0f);
    return c;
}

bool WriteConfig(const Config& c) {
    const std::wstring path = ConfigPath(true);
    if (path.empty()) return false;
    auto put = [&](const wchar_t* s, const wchar_t* k, const wchar_t* v) {
        return WritePrivateProfileStringW(s, k, v, path.c_str()) != FALSE;
    };
    auto putF = [&](const wchar_t* s, const wchar_t* k, float v) {
        wchar_t buf[32];
        swprintf(buf, 32, L"%.2f", v);
        return put(s, k, buf);
    };
    // v1 key: the padded box no longer exists.
    WritePrivateProfileStringW(L"RadarFrame", L"Padding", nullptr, path.c_str());
    wchar_t ver[8];
    swprintf(ver, 8, L"%d", kConfigVersion);
    return put(L"Meta", L"Version", ver) &&
           put(L"RadarFrame", L"Enabled", c.frameEnabled ? L"1" : L"0") &&
           put(L"Vitals", L"Enabled", c.vitalsEnabled ? L"1" : L"0") &&
           put(L"Vitals", L"ShowNumbers", c.showNumbers ? L"1" : L"0") &&
           putF(L"Vitals", L"OffsetX", c.offsetX) &&
           putF(L"Vitals", L"OffsetY", c.offsetY) &&
           putF(L"Vitals", L"Scale", c.scale) &&
           putF(L"Vitals", L"Spacing", c.spacing) &&
           put(L"Debug", L"Geometry", c.debugGeometry ? L"1" : L"0") &&
           put(L"Debug", L"Overlay", c.debugOverlay ? L"1" : L"0");
}

bool FileStamp(const std::wstring& path, FILETIME& out) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (path.empty() || !GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
        return false;
    out = data.ftLastWriteTime;
    return true;
}

void LoadConfig() {
    const std::wstring path = ConfigPath(false);
    Config c;   // defaults
    FILETIME stamp{};
    if (!FileStamp(path, stamp)) {
        // First run: write the defaults so the file exists for editing.
        WriteConfig(c);
        FileStamp(ConfigPath(false), stamp);
    } else {
        const int version = GetPrivateProfileIntW(L"Meta", L"Version", 1, path.c_str());
        c.frameEnabled  = GetPrivateProfileIntW(L"RadarFrame", L"Enabled", 1, path.c_str()) != 0;
        c.vitalsEnabled = GetPrivateProfileIntW(L"Vitals", L"Enabled", 1, path.c_str()) != 0;
        c.showNumbers   = GetPrivateProfileIntW(L"Vitals", L"ShowNumbers", 0, path.c_str()) != 0;
        c.offsetX       = ReadFloat(L"Vitals", L"OffsetX", c.offsetX, -200.0f, 200.0f, path);
        c.offsetY       = ReadFloat(L"Vitals", L"OffsetY", c.offsetY, -200.0f, 200.0f, path);
        c.scale         = ReadFloat(L"Vitals", L"Scale", c.scale, 0.5f, 2.0f, path);
        c.spacing       = ReadFloat(L"Vitals", L"Spacing", c.spacing, 0.0f, 20.0f, path);
        c.debugGeometry = GetPrivateProfileIntW(L"Debug", L"Geometry", 0, path.c_str()) != 0;
        c.debugOverlay  = GetPrivateProfileIntW(L"Debug", L"Overlay", 0, path.c_str()) != 0;
        if (version < kConfigVersion) {
            // v1 auto-wrote ShowNumbers=1 / Spacing=3 as defaults: reset those
            // to the v2 defaults, keep everything else the user set.
            const Config d;
            c.showNumbers = d.showNumbers;
            if (c.spacing == 3.0f) c.spacing = d.spacing;
            WriteConfig(Clamp(c));
            FileStamp(path, stamp);
            Log("config: migrated hud.ini v%d -> v%d", version, kConfigVersion);
        }
    }
    g_config = Clamp(c);
    g_configStamp = stamp;
    g_configLoaded = true;
    Log("config: outline=%d vitals=%d numbers=%d off=%.1f,%.1f scale=%.2f spacing=%.2f",
        g_config.frameEnabled, g_config.vitalsEnabled, g_config.showNumbers,
        g_config.offsetX, g_config.offsetY, g_config.scale, g_config.spacing);
}

void MaybeReloadConfig() {
    const DWORD now = GetTickCount();
    if (g_configLoaded && now - g_lastConfigCheck < 1000) return;
    g_lastConfigCheck = now;
    if (!g_configLoaded) { LoadConfig(); return; }
    FILETIME stamp{};
    if (FileStamp(ConfigPath(false), stamp) && CompareFileTime(&stamp, &g_configStamp) != 0)
        LoadConfig();
}

// ── Health/armour call-site redirection ─────────────────────────────
bool VitalsReplaced() {
    // Default (Round) mode: vanilla bars, no SKR strip.
    // Square/Custom mode: SKR strip replaces vanilla bars once the radar rect is known.
    return g_squareMode && g_hooksOk && g_config.vitalsEnabled && g_radarKnown;
}

bool EntryHidden(uintptr_t fn) {
    return *reinterpret_cast<const uint8_t*>(fn) == 0xC3;   // omp-cef HUD toggle
}

void __cdecl HookedRenderHealthBar(int playerId, int x, int y) {
    if (!VitalsReplaced() || playerId != 0) {
        reinterpret_cast<RenderBarFn>(kRenderHealthBar)(playerId, x, y);
        return;
    }
    g_vitalsFrame = g_hudFrame;   // GTA wants player info drawn this frame
}

void __cdecl HookedRenderArmorBar(int playerId, int x, int y) {
    if (!VitalsReplaced() || playerId != 0) {
        reinterpret_cast<RenderBarFn>(kRenderArmorBar)(playerId, x, y);
        return;
    }
    // Drawn by the custom block.
}

int RedirectCalls(uintptr_t target, void* hook) {
    int patched = 0;
    for (uintptr_t site = kDrawPlayerInfo; site + 5 <= kHudDraw; ++site) {
        auto* p = reinterpret_cast<uint8_t*>(site);
        if (p[0] != 0xE8) continue;
        if (site + 5 + *reinterpret_cast<int32_t*>(p + 1) != target) continue;
        const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(hook) - (site + 5));
        DWORD old = 0;
        if (!VirtualProtect(p + 1, 4, PAGE_EXECUTE_READWRITE, &old)) continue;
        *reinterpret_cast<int32_t*>(p + 1) = rel;
        VirtualProtect(p + 1, 4, old, &old);
        FlushInstructionCache(GetCurrentProcess(), p, 5);
        Log("redirected call at 0x%08X (target 0x%08X)", static_cast<unsigned>(site),
            static_cast<unsigned>(target));
        ++patched;
        site += 4;
    }
    return patched;
}

// Runs once on the GTA thread at the start of a HUD pass, i.e. while
// DrawPlayerInfo cannot be executing.
void InstallVitalsHooks() {
    g_hooksTried = true;
    const int health = RedirectCalls(kRenderHealthBar, reinterpret_cast<void*>(&HookedRenderHealthBar));
    const int armour = health > 0
        ? RedirectCalls(kRenderArmorBar, reinterpret_cast<void*>(&HookedRenderArmorBar)) : 0;
    g_hooksOk = health > 0;
    Log("vitals hooks: health sites=%d armour sites=%d -> %s", health, armour,
        g_hooksOk ? "custom block active" : "not found, vanilla bars kept");
}

// ── Drawing helpers (GTA 2D pass) ───────────────────────────────────
void Rect(float x1, float y1, float x2, float y2, CRGBA c) {
    if (x2 <= x1 || y2 <= y1) return;
    const CRect r = { x1, y1, x2, y2 };
    reinterpret_cast<DrawRectFn>(kSprite2dDrawRect)(r, c);
}

void Outline(float x1, float y1, float x2, float y2, float t, CRGBA c) {
    Rect(x1, y1, x2, y1 + t, c);
    Rect(x1, y2 - t, x2, y2, c);
    Rect(x1, y1 + t, x1 + t, y2 - t, c);
    Rect(x2 - t, y1 + t, x2, y2 - t, c);
}

void PrintNumber(float x, float y, float scaleX, float scaleY, int value) {
    char text[16];
    snprintf(text, sizeof(text), "%d", value);
    reinterpret_cast<void(__cdecl*)(short)>(kFontSetStyle)(kFontSubtitles);
    reinterpret_cast<void(__cdecl*)(float, float)>(kFontSetScale)(scaleX, scaleY);
    reinterpret_cast<void(__cdecl*)(CRGBA)>(kFontSetColor)(kText);
    reinterpret_cast<void(__cdecl*)(bool)>(kFontSetProportional)(true);
    reinterpret_cast<void(__cdecl*)(bool, bool)>(kFontSetBackground)(false, false);
    reinterpret_cast<void(__cdecl*)(bool)>(kFontSetJustify)(false);
    reinterpret_cast<void(__cdecl*)(int)>(kFontSetOrientation)(kAlignLeft);
    reinterpret_cast<void(__cdecl*)(float)>(kFontSetWrapx)(
        static_cast<float>(*reinterpret_cast<int*>(kRsMaxWidth)));
    reinterpret_cast<void(__cdecl*)(short)>(kFontSetDropShadow)(0);
    reinterpret_cast<void(__cdecl*)(short)>(kFontSetEdge)(1);
    reinterpret_cast<void(__cdecl*)(CRGBA)>(kFontSetDropColor)(CRGBA{0, 0, 0, 220});
    reinterpret_cast<void(__cdecl*)(float, float, char*)>(kFontPrintString)(x, y, text);
    reinterpret_cast<void(__cdecl*)(short)>(kFontSetEdge)(0);   // leave edge off for GTA
}

struct Vitals { bool ok; float health, maxHealth, armour; };

Vitals ReadVitals() {
    Vitals v{};
    __try {
        auto* ped = *reinterpret_cast<uint8_t**>(kPlayers);
        if (!ped) return v;
        v.health    = *reinterpret_cast<float*>(ped + kPedHealth);
        v.maxHealth = *reinterpret_cast<float*>(ped + kPedMaxHealth);
        v.armour    = *reinterpret_cast<float*>(ped + kPedArmour);
        v.ok = std::isfinite(v.health) && std::isfinite(v.armour);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        v.ok = false;
    }
    return v;
}

float Snap(float v) { return std::floor(v + 0.5f); }

// Thin bar: dark track over the full width, fill from the left. A zero value
// leaves just the track, so an empty armour bar stays a clean, quiet strip.
void DrawBar(float x, float y, float w, float h, float fraction, CRGBA fg, CRGBA bg) {
    const float x1 = Snap(x), y1 = Snap(y), x2 = Snap(x + w);
    const float y2 = (std::max)(y1 + 1.0f, Snap(y + h));
    Rect(x1, y1, x2, y2, bg);
    const float f = std::clamp(fraction, 0.0f, 1.0f);
    if (f > 0.0f) Rect(x1, y1, (std::max)(x1 + 1.0f, Snap(x1 + (x2 - x1) * f)), y2, fg);
}

void LogGeometry(float W, float H) {
    const DWORD now = GetTickCount();
    if (now - g_lastGeometryLog < 2000) return;
    g_lastGeometryLog = now;
    float rr[4] = {};
    unsigned vpW = 0, vpH = 0;
    __try {
        memcpy(rr, reinterpret_cast<const void*>(kRadarRect), sizeof(rr));
        auto* dev = *reinterpret_cast<IDirect3DDevice9**>(kRwD3D9Device);
        D3DVIEWPORT9 vp{};
        if (dev && SUCCEEDED(dev->GetViewport(&vp))) { vpW = vp.Width; vpH = vp.Height; }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    const RadarGeometry& g = g_geometry;
    Log("geometry: RsGlobal=%.0fx%.0f hudScale sx=%.4f sy=%.4f viewport=%ux%u | "
        "src w=%.2f h=%.2f topFromBottom=%.2f | formula=(%.1f,%.1f %.1fx%.1f) | "
        "measured=(%.1f,%.1f %.1fx%.1f) | m_radarRect=(%.1f,%.1f,%.1f,%.1f) | "
        "strip=(%.1f,%.1f %.1fx%.1f)",
        W, H, W / kHudBaseWidth, H / kHudBaseHeight, vpW, vpH,
        g.srcWidth, g.srcHeight, g.srcTopFromBottom,
        g.formulaX, g.formulaY, g.formulaW, g.formulaH,
        g.x, g.y, g.w, g.h, rr[0], rr[1], rr[2], rr[3],
        g_stripX, g_stripY, g_stripW, g_stripH);
}

void Draw() {
    const float W = static_cast<float>(*reinterpret_cast<int*>(kRsMaxWidth));
    const float H = static_cast<float>(*reinterpret_cast<int*>(kRsMaxHeight));
    if (W <= 0.0f || H <= 0.0f) return;
    const float sx = W / kHudBaseWidth;
    const float sy = H / kHudBaseHeight;
    const Config& c = g_config;

    const bool radarFresh = g_radarFrame == g_hudFrame;
    const bool vitalsRequested = VitalsReplaced() && radarFresh &&
                                 g_hudFrame - g_vitalsFrame <= 1u;
    const bool showHealth = vitalsRequested && !EntryHidden(kRenderHealthBar);
    const bool showArmour = vitalsRequested && !EntryHidden(kRenderArmorBar);
    const int rows = (showHealth ? 1 : 0) + (showArmour ? 1 : 0);

    const float rx = g_radarX, ry = g_radarY, rw = g_radarW, rh = g_radarH;
    const float bt = (g_squareMode && radarFresh) ? (std::max)(1.0f, Snap(kBorderPx)) : 0.0f;

    // ── Combined frame: radar map + vitals as one HUD component ──
    // When vitals are visible: outer border wraps the entire assembly
    // (map + separator + bars). A 1px separator line divides the map area
    // from the vitals row. When vitals are hidden: border around radar only.
    if (bt > 0.0f && rows > 0) {
        const float margin = kScreenMargin * sy;
        float k = c.scale;
        const float barH = (std::max)(2.0f, kBarH * k * sy);
        const float sep = bt;   // separator = same thickness as the border

        // Vitals Y: immediately below the map + separator, inside the frame
        float vx = rx + c.offsetX * sx;
        float vy = ry + rh + sep;
        const float vw = rw;
        vx = std::clamp(vx, 0.0f, (std::max)(0.0f, W - vw));

        // Clamp if the assembly would extend below the screen
        const float componentBottom = vy + barH + bt;
        if (componentBottom > H - margin) {
            k = (std::max)(0.5f, (H - margin - vy - bt) / (kBarH * sy));
        }
        const float finalBarH = (std::max)(2.0f, kBarH * k * sy);

        // Outer frame around the full assembly: radar + separator + vitals
        const float frameTop    = ry - bt;
        const float frameLeft   = rx - bt;
        const float frameRight  = rx + rw + bt;
        const float frameBottom = vy + finalBarH + bt;
        Outline(frameLeft, frameTop, frameRight, frameBottom, bt, kBorder);

        // Separator between map and vitals (inside the frame)
        Rect(rx, ry + rh, rx + rw, ry + rh + sep, kBorder);

        const Vitals v = ReadVitals();
        if (v.ok) {
            const float gap = (std::max)(1.0f, Snap(kBarGap * sx));
            const float maxHealth = v.maxHealth > 1.0f ? v.maxHealth : 100.0f;
            const float half = (rows == 2) ? Snap((vw - gap) * 0.5f) : vw;
            const float ax = (rows == 2) ? vx + half + gap : vx;
            const float aw = (rows == 2) ? vw - half - gap : vw;
            if (showHealth)
                DrawBar(vx, vy, half, finalBarH, v.health / maxHealth, kHealth, kHealthBg);
            if (showArmour)
                DrawBar(ax, vy, aw, finalBarH, v.armour / 100.0f, kArmour, kArmourBg);
            g_stripX = vx; g_stripY = vy; g_stripW = vw; g_stripH = finalBarH;

            const bool showNumbers = g_serverVitalsNumbers >= 0 ? g_serverVitalsNumbers != 0
                                                                : c.showNumbers;
            if (showNumbers) {
                const float tsx = kTextScaleX * k * sx;
                const float tsy = kTextScaleY * k * sy;
                const float textH = kTextUnitsPerScaleY * kTextScaleY * k * sy;
                const float ty = vy + (finalBarH - textH) * 0.5f;
                float nx = vx + vw + kNumberGap * k * sx;
                if (showHealth) {
                    PrintNumber(nx, ty, tsx, tsy, static_cast<int>(std::ceil((std::max)(0.0f, v.health))));
                    nx += kNumberW * k * sx;
                }
                if (showArmour)
                    PrintNumber(nx, ty, tsx, tsy, static_cast<int>(std::ceil((std::max)(0.0f, v.armour))));
            }
        }
    } else if (bt > 0.0f) {
        // No vitals: just the radar frame
        Outline(rx - bt, ry - bt, rx + rw + bt, ry + rh + bt, bt, kBorder);
    }

    if (c.debugOverlay && g_radarKnown) {
        Outline(rx, ry, rx + rw, ry + rh, 1.0f, kDebugRadar);
        if (g_stripW > 0.0f)
            Outline(g_stripX, g_stripY, g_stripX + g_stripW, g_stripY + g_stripH, 1.0f, kDebugStrip);
    }
    if (c.debugGeometry && g_radarKnown) LogGeometry(W, H);
}

} // namespace

bool ReadPlayerVitals(float& health, float& maxHealth, float& armour) {
    const Vitals v = ReadVitals();
    if (!v.ok) return false;
    health = v.health;
    maxHealth = v.maxHealth > 1.0f ? v.maxHealth : 100.0f;
    armour = v.armour;
    return true;
}

Config GetConfig() {
    if (!g_configLoaded) LoadConfig();
    return g_config;
}

bool SetConfig(const Config& config) {
    g_config = Clamp(config);
    g_configLoaded = true;
    const bool ok = WriteConfig(g_config);
    FileStamp(ConfigPath(false), g_configStamp);
    return ok;
}

void SetServerVitalsNumbers(int value) {
    g_serverVitalsNumbers = (value < 0) ? -1 : (value ? 1 : 0);
}

void SetSquareMode(bool square) {
    g_squareMode = square;
}

void OnHudDrawBegin() {
    ++g_hudFrame;
    MaybeReloadConfig();
    if (!g_hooksTried) InstallVitalsHooks();
}

void OnRadarDrawn(const RadarGeometry& geometry) {
    if (!(geometry.w > 0.0f && geometry.h > 0.0f)) return;
    g_geometry = geometry;
    g_radarX = geometry.x; g_radarY = geometry.y;
    g_radarW = geometry.w; g_radarH = geometry.h;
    g_radarKnown = true;
    g_radarFrame = g_hudFrame;
}

void OnHudDrawEnd() {
    if (!g_configLoaded) return;
    Draw();
}

} // namespace skr::radar_hud
