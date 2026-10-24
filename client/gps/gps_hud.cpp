// FALLBACK navigation status line above the minimap, drawn with GTA's CFont
// in the same HUD 2D pass as radar_hud (CHud::Draw end). Since API-003.2 the
// ImGui panel (gps_panel.h) is the navigation display; this line draws only
// while that renderer is not available, so the two never show together.
// Addresses verified in radar_hud.cpp (plugin-sdk CFont.cpp). Text scale
// values are visual estimates.

#include "gps_feedback.h"
#include "gps_flags.h"
#include "gps_renderer.h"
#include "native-pause/pause_renderer.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>

namespace skr::gps {
namespace {

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
constexpr uintptr_t kRsMaxWidth          = 0xC17044;
constexpr uintptr_t kRsMaxHeight         = 0xC17048;
constexpr short     kFontSubtitles       = 1;
constexpr int       kAlignLeft           = 1;

struct CRGBA { uint8_t r, g, b, a; };
constexpr CRGBA kLime  = {168, 238, 101, 255};
constexpr CRGBA kWhite = {235, 240, 240, 255};
constexpr CRGBA kAmber = {240, 200, 80, 255};

void FormatDistance(char* out, size_t len, float metres) {
    if (metres < 1000.0f) snprintf(out, len, "%d m", static_cast<int>(metres / 10.0f + 0.5f) * 10);
    else                  snprintf(out, len, "%.1f km", metres / 1000.0f);
}

bool Print(float x, float y, float sx, float sy, CRGBA color, char* text) {
    __try {
        reinterpret_cast<void(__cdecl*)(short)>(kFontSetStyle)(kFontSubtitles);
        reinterpret_cast<void(__cdecl*)(float, float)>(kFontSetScale)(sx, sy);
        reinterpret_cast<void(__cdecl*)(CRGBA)>(kFontSetColor)(color);
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
        reinterpret_cast<void(__cdecl*)(short)>(kFontSetEdge)(0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

void DrawStatus() {
    if (!flags::RendererEnabled()) return;
    if (native_pause::renderer::IsReady()) return;   // ImGui panel owns the display
    float rx, ry, rw, rh;
    if (!LastRadarRect(rx, ry, rw, rh)) return;
    const feedback::Summary& s = feedback::GetSummary();
    const DWORD age = GetTickCount() - s.stateSince;

    char line1[96] = {}, line2[96] = {}, dist[32] = {};
    CRGBA color1 = kWhite;
    using State = feedback::Summary::State;
    switch (s.state) {
        case State::Navigating:
            FormatDistance(dist, sizeof(dist), s.remaining);
            snprintf(line1, sizeof(line1), "GPS  %s", dist);
            if (s.next.reliable && s.next.maneuver != feedback::Maneuver::None) {
                char d2[32];
                FormatDistance(d2, sizeof(d2), s.next.distance);
                const char* text = "";
                switch (s.next.maneuver) {
                    case feedback::Maneuver::Arrive:      text = "Destination"; break;
                    case feedback::Maneuver::SlightLeft:  text = "Keep left"; break;
                    case feedback::Maneuver::SlightRight: text = "Keep right"; break;
                    case feedback::Maneuver::Left:        text = "Turn left"; break;
                    case feedback::Maneuver::Right:       text = "Turn right"; break;
                    case feedback::Maneuver::SharpLeft:   text = "Sharp left"; break;
                    case feedback::Maneuver::SharpRight:  text = "Sharp right"; break;
                    case feedback::Maneuver::UTurn:       text = "Make a U-turn"; break;
                    default: break;
                }
                snprintf(line2, sizeof(line2), "%s in %s", text, d2);
            }
            break;
        case State::Recalculating: snprintf(line1, sizeof(line1), "GPS  Recalculating..."); color1 = kAmber; break;
        case State::Calculating:   snprintf(line1, sizeof(line1), "GPS  Calculating route..."); break;
        case State::Arrived:       snprintf(line1, sizeof(line1), "GPS  Arrived"); color1 = kLime; break;
        case State::Unavailable:
            if (age > 10000) return;
            snprintf(line1, sizeof(line1), "GPS  %s", ToString(s.failure));
            color1 = kAmber;
            break;
        default: return;
    }

    const float screenW = static_cast<float>(*reinterpret_cast<int*>(kRsMaxWidth));
    const float screenH = static_cast<float>(*reinterpret_cast<int*>(kRsMaxHeight));
    if (screenW <= 0 || screenH <= 0) return;
    const float sx = 0.20f * screenW / 640.0f, sy = 0.38f * screenH / 448.0f;
    const float lineH = 16.0f * sy + 2.0f;   // CFont glyph height ≈ 16 × scaleY
    const float y2 = ry - lineH - 4.0f;
    const float y1 = line2[0] ? y2 - lineH : y2;
    Print(rx, y1, sx, sy, color1, line1);
    if (line2[0]) Print(rx, y2, sx * 0.9f, sy * 0.9f, kWhite, line2);
}

} // namespace skr::gps
