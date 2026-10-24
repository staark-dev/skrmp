#pragma once
// ═══════════════════════════════════════════════════════════════════════
// GPS navigation panel — pure layout + presentation model (API-003.2)
// ═══════════════════════════════════════════════════════════════════════
// No ImGui, no D3D9, no GTA memory: everything here is deterministic and
// unit-tested (tests/unit/test_gps_panel.cpp). gps_panel.cpp turns a View +
// Layout into ImGui draw-list commands.
//
// Geometry: the panel sits directly above the measured minimap rect, same
// left edge and width, a 4–6 px gap. Heights and font sizes come from one
// unit (back-buffer height / 768 × UI scale), applied exactly once here.

#include "gps_display_policy.h"
#include "gps_feedback.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace skr::gps::panel {

struct Rect {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    float Width() const  { return x1 - x0; }
    float Height() const { return y1 - y0; }
};

struct LayoutInput {
    Rect  radar;               // measured minimap rect (screen px)
    float screenW = 0, screenH = 0;
    float uiScale = 1.0f;      // ESC menu UI Scale setting
    float topSafe = -1.0f;     // panel never above this y (chat area); < 0 = 30 % of screen height
};

struct Layout {
    bool  visible = false;     // false: no room above the radar / invalid input
    float unit = 1.0f;
    Rect  panel, accent, row1, row2;
    float padLeft = 0, padRight = 0;  // text inset inside row1/row2
    float labelFont = 0, valueFont = 0, detailFont = 0;   // px
    float rounding = 0, shadow = 0, gap = 0;
};

inline float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float Snap(float v) { return std::floor(v + 0.5f); }

inline Layout ComputeLayout(const LayoutInput& in) {
    Layout L;
    const float rw = in.radar.Width(), rh = in.radar.Height();
    if (!(rw > 8.0f && rh > 8.0f && in.screenW > 0.0f && in.screenH > 0.0f)) return L;

    L.unit = (in.screenH / 768.0f) * Clamp(in.uiScale, 0.8f, 1.4f);
    const float u = L.unit;
    L.gap = Clamp(Snap(5.0f * u), 4.0f, 6.0f);
    const float padY = Snap(4.0f * u);
    // Row 2 is 17 u so the 13 u location text (live feedback: 12 u was hard to
    // read in motion at 1360x768) keeps 2 px above and below.
    const float row1H = Snap(17.0f * u), row2H = Snap(17.0f * u);
    const float height = padY + row1H + row2H + padY;

    // Left edge and width follow the minimap exactly, kept on screen.
    float x0 = Snap(in.radar.x0), x1 = Snap(in.radar.x1);
    if (x0 < 0.0f) { x1 -= x0; x0 = 0.0f; }
    if (x1 > in.screenW) { x0 -= x1 - in.screenW; x1 = in.screenW; }
    if (x0 < 0.0f) x0 = 0.0f;
    const float y1 = Snap(in.radar.y0) - L.gap;
    const float y0 = y1 - height;
    const float topSafe = in.topSafe >= 0.0f ? in.topSafe : in.screenH * 0.30f;
    if (y0 < topSafe || y1 > in.screenH) return L;

    L.panel  = {x0, y0, x1, y1};
    L.accent = {x0, y0, x0 + (std::max)(2.0f, Snap(2.0f * u)), y1};
    L.row1   = {x0, y0 + padY, x1, y0 + padY + row1H};
    L.row2   = {x0, L.row1.y1, x1, L.row1.y1 + row2H};
    L.padLeft  = L.accent.Width() + Snap(7.0f * u);
    L.padRight = Snap(7.0f * u);
    L.labelFont  = Snap(12.0f * u);
    L.valueFont  = Snap(14.0f * u);
    L.detailFont = Snap(13.0f * u);
    L.rounding = Snap(2.0f * u);
    L.shadow   = Snap(3.0f * u);
    L.visible = L.panel.Width() > L.padLeft + L.padRight + 8.0f;
    return L;
}

// ── Single-line text placement inside a rect (CSS-equivalent: left / right,
//    vertical-align: middle). Measured sizes come from the active font.
struct TextPos { float x, y; };
inline TextPos AlignLeft(const Rect& r, float padLeft, float /*textW*/, float textH) {
    return {Snap(r.x0 + padLeft), Snap(r.y0 + (r.Height() - textH) * 0.5f)};
}
inline TextPos AlignRight(const Rect& r, float padRight, float textW, float textH) {
    return {Snap(r.x1 - padRight - textW), Snap(r.y0 + (r.Height() - textH) * 0.5f)};
}

// ── Compact (on-foot) distance box: inside the radar, centred, near its top
//    edge. For the round radar (an ellipse at widescreen, like GTA's) the box
//    is lowered just enough that both top corners stay inside the ellipse.
struct CompactLayout {
    bool  visible = false;
    Rect  box;
    float font = 0, padX = 0, rounding = 0;
};

inline CompactLayout ComputeCompactLayout(const LayoutInput& in, bool roundRadar, float textW) {
    CompactLayout C;
    const float rw = in.radar.Width(), rh = in.radar.Height();
    if (!(rw > 8.0f && rh > 8.0f && in.screenH > 0.0f && textW >= 0.0f)) return C;
    const float u = (in.screenH / 768.0f) * Clamp(in.uiScale, 0.8f, 1.4f);
    C.font = Snap(11.0f * u);
    C.padX = Snap(5.0f * u);
    C.rounding = Snap(2.0f * u);
    const float w = Snap(textW + 2.0f * C.padX), h = Snap(15.0f * u), margin = (std::max)(2.0f, Snap(3.0f * u));
    const float cx = (in.radar.x0 + in.radar.x1) * 0.5f, cy = (in.radar.y0 + in.radar.y1) * 0.5f;
    float top = in.radar.y0 + margin;
    if (roundRadar) {
        const float a = rw * 0.5f, b = rh * 0.5f, hw = w * 0.5f + margin;
        if (hw >= a) return C;
        const float yEdge = b * std::sqrt(1.0f - (hw / a) * (hw / a));   // ellipse half-height at |x| = hw
        top = cy - yEdge + margin;
    }
    if (w > rw - 2.0f * margin || top + h > cy) return C;   // never reaches the player arrow
    C.box = {Snap(cx - w * 0.5f), Snap(top), Snap(cx - w * 0.5f) + w, Snap(top) + h};
    C.visible = true;
    return C;
}

// Longest prefix of `text` that fits `maxWidth` with "..." appended (UTF-8
// safe: never splits a multi-byte sequence; trailing spaces / separators are
// dropped). Fits unchanged → copied as is. measure(text, len) = px width.
template <class Measure>
inline void FitText(const char* text, float maxWidth, Measure measure, char* out, size_t outLen) {
    if (!out || outLen == 0) return;
    out[0] = '\0';
    if (!text) return;
    const size_t len = std::strlen(text);
    if (measure(text, len) <= maxWidth) { snprintf(out, outLen, "%s", text); return; }
    const float dots = measure("...", 3);
    size_t lo = 0, hi = len;                 // largest n with width(n) + dots <= max
    while (lo < hi) {
        const size_t mid = (lo + hi + 1) / 2;
        if (measure(text, mid) + dots <= maxWidth) lo = mid; else hi = mid - 1;
    }
    size_t n = lo;
    while (n > 0 && (static_cast<unsigned char>(text[n]) & 0xC0) == 0x80) --n;   // inside a UTF-8 char
    auto isTrim = [&](size_t i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        return c == ' ' || c == ',' || c == '-' ||
               (c == 0xC2 && i + 1 < len && static_cast<unsigned char>(text[i + 1]) == 0xB7);
    };
    while (n > 0) {
        if (isTrim(n - 1)) { --n; continue; }
        if (n >= 2 && isTrim(n - 2) && static_cast<unsigned char>(text[n - 2]) == 0xC2) { n -= 2; continue; }
        break;
    }
    if (n == 0) { snprintf(out, outLen, "%s", dots <= maxWidth ? "..." : ""); return; }
    snprintf(out, outLen, "%.*s...", static_cast<int>(n), text);
}

// ── Distance text with hysteresis: 10 m steps below 1 km ("290 M"), 0.1 km
//    above ("3.3 KM"). A step changes only once the raw value is 35 % of a
//    step past the rounding boundary (3.5 m / 35 m), so noisy distances do
//    not flicker;
//    metres → km at 1000 m, back below 950 m.
class DistanceDisplay {
public:
    void Reset() { m_shown = -1.0f; m_km = false; }
    void Update(float metres) {
        if (!(metres >= 0.0f)) return;
        if (m_shown < 0.0f) { m_km = metres >= 1000.0f; m_shown = Quantize(metres); return; }
        if (!m_km && metres >= 1000.0f) { m_km = true; m_shown = Quantize(metres); return; }
        if (m_km && metres < 950.0f)    { m_km = false; m_shown = Quantize(metres); return; }
        const float q = Step();
        if (std::fabs(metres - m_shown) > q * 0.85f) m_shown = Quantize(metres);
    }
    bool  Valid() const { return m_shown >= 0.0f; }
    float Shown() const { return m_shown; }
    void Format(char* out, size_t len) const {
        if (!Valid()) { snprintf(out, len, "--"); return; }
        if (m_km) snprintf(out, len, "%.1f KM", m_shown / 1000.0f);
        else      snprintf(out, len, "%d M", static_cast<int>(m_shown < 10.0f ? 10.0f : m_shown));
    }
private:
    float Step() const { return m_km ? 100.0f : 10.0f; }
    float Quantize(float m) const { const float q = Step(); return std::floor(m / q + 0.5f) * q; }
    float m_shown = -1.0f;
    bool  m_km = false;
};

// "Area · City", "City" or "Unknown area" from GTA zone names. Only
// printable ASCII is kept (GXT zone text; anything else would not be UTF-8).
inline void FormatLocation(const char* area, const char* city, char* out, size_t len) {
    char a[48] = {}, c[48] = {};
    auto clean = [](const char* in, char* dst, size_t n) {
        size_t k = 0;
        for (; in && *in && k + 1 < n; ++in)
            if (*in >= 0x20 && *in < 0x7F) dst[k++] = *in;
        while (k > 0 && dst[k - 1] == ' ') --k;
        dst[k] = '\0';
    };
    clean(area, a, sizeof(a));
    clean(city, c, sizeof(c));
    if (a[0] && c[0] && std::strcmp(a, c) != 0) snprintf(out, len, "%s \xC2\xB7 %s", a, c);
    else if (a[0])                              snprintf(out, len, "%s", a);
    else if (c[0])                              snprintf(out, len, "%s", c);
    else                                        snprintf(out, len, "Unknown area");
}

// ── Presentation state ─────────────────────────────────────────────────
enum class Tone : uint8_t { Normal, Pending, Success, Error };

// What the game tick knows about the player (see gps_panel.cpp).
struct NavContext {
    Seat  seat = Seat::Driver;
    bool  hasWaypoint = true;
    float waypointDistance = -1.0f;   // straight line (2D), < 0 = unknown
};

struct CompactView {
    bool  visible = false;
    float alpha = 0.0f;
    char  value[24] = {};             // "290 M"
};

struct View {
    bool  visible = false;
    float alpha = 0.0f;        // fade in/out
    Tone  tone = Tone::Normal;
    char  label[32] = {};      // row 1, left  ("GPS ROUTE")
    char  value[24] = {};      // row 1, right ("3.3 KM"), may be empty
    bool  valueDimmed = false; // last known distance while recalculating
    char  detail[96] = {};     // row 2, left  (location / maneuver / failure)
};

constexpr uint32_t kArrivedShowMs = 3500;
constexpr uint32_t kErrorShowMs   = 6000;
constexpr uint32_t kFadeMs        = 160;
constexpr float    kManeuverShowM = 250.0f;   // turn text replaces the location this close

inline const char* ManeuverText(feedback::Maneuver m) {
    switch (m) {
        case feedback::Maneuver::SlightLeft:  return "Keep left";
        case feedback::Maneuver::SlightRight: return "Keep right";
        case feedback::Maneuver::Left:        return "Turn left";
        case feedback::Maneuver::Right:       return "Turn right";
        case feedback::Maneuver::SharpLeft:   return "Sharp left";
        case feedback::Maneuver::SharpRight:  return "Sharp right";
        case feedback::Maneuver::UTurn:       return "Make a U-turn";
        default:                              return nullptr;
    }
}

inline const char* FailureText(RouteFailure f) {
    switch (f) {
        case RouteFailure::None:  return "Route unavailable";
        default:                  return ToString(f);
    }
}

// Converts the engine's Summary into what the panel shows. Update() is cheap
// (no I/O) and runs once per drawn frame.
class Model {
public:
    void Reset() { *this = Model(); }
    void SetLocation(const char* area, const char* city) {
        FormatLocation(area, city, m_location, sizeof(m_location));
        m_hasLocation = true;
    }
    void SetOverride(ModeOverride o) { m_policy.SetOverride(o); }
    DisplayMode Mode() const { return m_policy.Current(); }

    // Driver context: the full panel exactly as before the display policy.
    void Update(const feedback::Summary& s, uint32_t now) { Update(s, NavContext(), now); }

    void Update(const feedback::Summary& s, const NavContext& ctx, uint32_t now) {
        using State = feedback::Summary::State;
        const uint32_t age = now - s.stateSince;
        const DisplayMode mode = m_policy.Update(s.state != State::Idle, ctx.seat, now);
        bool show = false;
        View v;
        switch (s.state) {
            case State::Navigating:
                m_distance.Update(s.remaining);
                show = true;
                snprintf(v.label, sizeof(v.label), "GPS ROUTE");
                m_distance.Format(v.value, sizeof(v.value));
                if (const char* turn = s.next.reliable && s.next.distance <= kManeuverShowM
                                           ? ManeuverText(s.next.maneuver) : nullptr) {
                    m_turn.Update(s.next.distance);
                    char d[24];
                    m_turn.Format(d, sizeof(d));
                    for (char* p = d; *p; ++p) if (*p >= 'A' && *p <= 'Z') *p = static_cast<char>(*p - 'A' + 'a');
                    snprintf(v.detail, sizeof(v.detail), "%s in %s", turn, d);
                } else {
                    m_turn.Reset();
                    snprintf(v.detail, sizeof(v.detail), "%s", Location());
                }
                break;
            case State::Calculating:
            case State::Recalculating:
                show = true;
                v.tone = Tone::Pending;
                snprintf(v.label, sizeof(v.label), s.state == State::Recalculating ? "RECALCULATING" : "CALCULATING");
                if (m_distance.Valid()) { m_distance.Format(v.value, sizeof(v.value)); v.valueDimmed = true; }
                snprintf(v.detail, sizeof(v.detail), "%s", Location());
                break;
            case State::Arrived:
                show = age < kArrivedShowMs;
                v.tone = Tone::Success;
                snprintf(v.label, sizeof(v.label), "ARRIVED");
                snprintf(v.detail, sizeof(v.detail), "%s", Location());
                break;
            case State::Unavailable:
                show = age < kErrorShowMs;
                v.tone = Tone::Error;
                snprintf(v.label, sizeof(v.label), "NO ROUTE");
                snprintf(v.detail, sizeof(v.detail), "%s", FailureText(s.failure));
                if (v.detail[0] >= 'a' && v.detail[0] <= 'z') v.detail[0] = static_cast<char>(v.detail[0] - 'a' + 'A');
                break;
            case State::Idle:
            default:
                break;
        }
        if (s.state == State::Idle || s.state == State::Unavailable) m_distance.Reset();

        // One widget at a time: the incoming one starts only after the outgoing
        // one has faded out completely (no overlap, no duplicate distance).
        show = show && mode == DisplayMode::Full && !m_compact.visible;
        UpdateCompact(s, ctx, mode, now);

        // Fade: in over kFadeMs from the first visible frame, out over kFadeMs
        // keeping the last content. Arrived/Unavailable fade before their end.
        if (show) {
            if (!m_shown) { m_shown = true; m_shownAt = now; }
            float a = static_cast<float>(now - m_shownAt) / kFadeMs;
            if (s.state == State::Arrived || s.state == State::Unavailable) {
                const uint32_t limit = s.state == State::Arrived ? kArrivedShowMs : kErrorShowMs;
                const float out = static_cast<float>(limit - age) / kFadeMs;
                if (out < a) a = out;
            }
            v.visible = true;
            v.alpha = Clamp(a, 0.0f, 1.0f);
            m_view = v;
            m_hiddenAt = 0;
        } else if (m_shown) {
            if (!m_hiddenAt) m_hiddenAt = now;
            const float a = 1.0f - static_cast<float>(now - m_hiddenAt) / kFadeMs;
            m_view.alpha = (std::min)(m_view.alpha, Clamp(a, 0.0f, 1.0f));
            if (m_view.alpha <= 0.0f) { m_shown = false; m_hiddenAt = 0; m_view = View(); }
        }
    }
    const View& GetView() const { return m_view; }
    const CompactView& GetCompact() const { return m_compact; }
    bool HasLocation() const { return m_hasLocation; }

private:
    void UpdateCompact(const feedback::Summary& s, const NavContext& ctx, DisplayMode mode, uint32_t now) {
        using State = feedback::Summary::State;
        const bool show = mode == DisplayMode::Compact && ctx.hasWaypoint && ctx.waypointDistance >= 0.0f &&
                          s.state != State::Idle && s.state != State::Arrived && !m_view.visible;
        if (show) {
            m_straight.Update(ctx.waypointDistance);
            if (!m_compactShown) { m_compactShown = true; m_compactShownAt = now; }
            m_compact.visible = true;
            m_compact.alpha = Clamp(static_cast<float>(now - m_compactShownAt) / kFadeMs, 0.0f, 1.0f);
            m_straight.Format(m_compact.value, sizeof(m_compact.value));
            m_compactHiddenAt = 0;
        } else if (m_compactShown) {
            if (!m_compactHiddenAt) m_compactHiddenAt = now;
            const float a = 1.0f - static_cast<float>(now - m_compactHiddenAt) / kFadeMs;
            m_compact.alpha = (std::min)(m_compact.alpha, Clamp(a, 0.0f, 1.0f));
            if (m_compact.alpha <= 0.0f) {
                m_compactShown = false; m_compactHiddenAt = 0; m_compact = CompactView(); m_straight.Reset();
            }
        }
        if (!ctx.hasWaypoint) m_straight.Reset();
    }

    // Empty until the first zone lookup (≤ 0.5 s), so "Unknown area" only
    // appears when GTA really reports no zone.
    const char* Location() const { return m_hasLocation ? m_location : ""; }
    DistanceDisplay m_distance, m_turn, m_straight;
    DisplayPolicy   m_policy;
    View     m_view;
    CompactView m_compact;
    bool     m_compactShown = false;
    uint32_t m_compactShownAt = 0, m_compactHiddenAt = 0;
    char     m_location[112] = "Unknown area";
    bool     m_hasLocation = false;
    bool     m_shown = false;
    uint32_t m_shownAt = 0, m_hiddenAt = 0;
};

} // namespace skr::gps::panel
