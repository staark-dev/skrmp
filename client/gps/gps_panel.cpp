#include "gps_panel.h"
#include "gps_destination.h"
#include "gps_flags.h"
#include "gps_panel_model.h"
#include "gps_renderer.h"

#include <imgui.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cfloat>
#include <cmath>

namespace skr::gps::panel {
namespace {

Model    g_model;
uint32_t g_drawnSequence = 0;
float    g_playerX = 0.0f, g_playerY = 0.0f;
bool     g_playerKnown = false;
Seat     g_seat = Seat::OnFoot;

ImU32 Col(int r, int g, int b, float a) {
    return IM_COL32(r, g, b, static_cast<int>(Clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f));
}

ImU32 ToneColor(Tone t, float a) {
    switch (t) {
        case Tone::Pending: return Col(240, 196, 90, a);
        case Tone::Error:   return Col(236, 110, 96, a);
        default:            return Col(163, 230, 91, a);   // #A3E65B, route colour
    }
}

} // namespace

void SetLocation(const char* area, const char* city) { g_model.SetLocation(area, city); }

bool WantsLocation() { return feedback::GetSummary().state != feedback::Summary::State::Idle; }

void SetPlayerContext(float x, float y, Seat seat) {
    g_playerX = x;
    g_playerY = y;
    g_playerKnown = true;
    g_seat = seat;
}

void SetDisplayOverride(ModeOverride mode) { g_model.SetOverride(mode); }

DisplayMode CurrentDisplayMode() { return g_model.Mode(); }

bool WantsRender() {
    if (!flags::RendererEnabled()) return false;
    // Engine state only (no GTA memory): the waypoint the engine navigates to.
    NavContext ctx;
    ctx.seat = g_seat;
    ctx.hasWaypoint = HasDestination();
    if (ctx.hasWaypoint && g_playerKnown) {
        const Vec3 wp = GetDestination();
        const float dx = wp.x - g_playerX, dy = wp.y - g_playerY;   // waypoint z is 0: 2D
        ctx.waypointDistance = std::sqrt(dx * dx + dy * dy);
    }
    g_model.Update(feedback::GetSummary(), ctx, GetTickCount());
    const RadarFrame radar = LastRadarFrame();
    return (g_model.GetView().visible || g_model.GetCompact().visible) && radar.valid &&
           radar.sequence != g_drawnSequence;
}

void DrawImGui(ImFont* heading, ImFont* body, float uiScale) {
    const View& v = g_model.GetView();
    const CompactView& c = g_model.GetCompact();
    const RadarFrame radar = LastRadarFrame();
    if ((!v.visible && !c.visible) || !radar.valid || radar.sequence == g_drawnSequence) return;
    g_drawnSequence = radar.sequence;

    const ImGuiIO& io = ImGui::GetIO();
    LayoutInput in;
    in.radar = {radar.x, radar.y, radar.x + radar.w, radar.y + radar.h};
    in.screenW = io.DisplaySize.x;
    in.screenH = io.DisplaySize.y;
    in.uiScale = uiScale;
    if (c.visible) {
        ImFont* fh = heading ? heading : ImGui::GetFont();
        CompactLayout probe = ComputeCompactLayout(in, !radar.square, 0.0f);
        const float textW = fh->CalcTextSizeA(probe.font, FLT_MAX, 0.0f, c.value).x;
        DrawCompact(ImGui::GetForegroundDrawList(), c, ComputeCompactLayout(in, !radar.square, textW), heading);
    }
    if (!v.visible) return;
    const Layout L = ComputeLayout(in);
    if (!L.visible) return;
    DrawPanel(ImGui::GetForegroundDrawList(), v, L, heading, body);
}

void DrawCompact(ImDrawList* dl, const CompactView& v, const CompactLayout& L, ImFont* heading) {
    if (!dl || !v.visible || !L.visible) return;
    ImFont* fh = heading ? heading : ImGui::GetFont();
    const float a = v.alpha;
    const ImVec2 p0(L.box.x0, L.box.y0), p1(L.box.x1, L.box.y1);
    dl->AddRectFilled(ImVec2(p0.x, p0.y + 1.0f), ImVec2(p1.x, p1.y + 1.0f), Col(0, 0, 0, 0.25f * a), L.rounding);
    dl->AddRectFilled(p0, p1, Col(11, 19, 31, 0.78f * a), L.rounding);
    dl->AddRect(p0, p1, Col(163, 230, 91, 0.35f * a), L.rounding, ImDrawFlags_None, 1.0f);
    const ImVec2 size = fh->CalcTextSizeA(L.font, FLT_MAX, 0.0f, v.value);
    const Rect r = L.box;
    const TextPos tp = {Snap(r.x0 + (r.Width() - size.x) * 0.5f), Snap(r.y0 + (r.Height() - size.y) * 0.5f)};
    dl->AddText(fh, L.font, ImVec2(tp.x, tp.y), Col(240, 244, 246, a), v.value);
}

void DrawPanel(ImDrawList* dl, const View& v, const Layout& L, ImFont* heading, ImFont* body) {
    if (!dl || !v.visible || !L.visible) return;
    ImFont* fh = heading ? heading : ImGui::GetFont();
    (void)body;   // both rows use the heading face since the readability fix
    const float a = v.alpha;
    const ImVec2 p0(L.panel.x0, L.panel.y0), p1(L.panel.x1, L.panel.y1);

    // Shadow, body, hairline border, accent.
    dl->AddRectFilled(ImVec2(p0.x - 1.0f, p0.y), ImVec2(p1.x + 1.0f, p1.y + L.shadow),
                      Col(0, 0, 0, 0.18f * a), L.rounding + 1.0f);
    dl->AddRectFilled(ImVec2(p0.x, p0.y + 1.0f), ImVec2(p1.x, p1.y + 1.0f), Col(0, 0, 0, 0.30f * a), L.rounding);
    dl->AddRectFilled(p0, p1, Col(11, 19, 31, 0.80f * a), L.rounding);
    dl->AddRect(p0, p1, Col(255, 255, 255, 0.09f * a), L.rounding, ImDrawFlags_None, 1.0f);
    dl->AddRectFilled(ImVec2(L.accent.x0, L.accent.y0), ImVec2(L.accent.x1, L.accent.y1),
                      ToneColor(v.tone, a), L.rounding, ImDrawFlags_RoundCornersLeft);

    // Row 1: label left, distance right, both vertically centred.
    char value[24];
    snprintf(value, sizeof(value), "%s", v.value);
    const ImVec2 valueSize = value[0] ? fh->CalcTextSizeA(L.valueFont, FLT_MAX, 0.0f, value) : ImVec2(0, 0);
    if (value[0]) {
        const TextPos tp = AlignRight(L.row1, L.padRight, valueSize.x, valueSize.y);
        dl->AddText(fh, L.valueFont, ImVec2(tp.x, tp.y),
                    v.valueDimmed ? Col(240, 244, 246, 0.50f * a) : Col(240, 244, 246, a), value);
    }
    const float labelMax = L.row1.Width() - L.padLeft - L.padRight - (value[0] ? valueSize.x + 6.0f * L.unit : 0.0f);
    char label[32];
    const float labelFont = L.labelFont;
    FitText(v.label, labelMax, [&](const char* t, size_t n) {
        return fh->CalcTextSizeA(labelFont, FLT_MAX, 0.0f, t, t + n).x;
    }, label, sizeof(label));
    const ImVec2 labelSize = fh->CalcTextSizeA(L.labelFont, FLT_MAX, 0.0f, label);
    const TextPos lp = AlignLeft(L.row1, L.padLeft, labelSize.x, labelSize.y);
    dl->AddText(fh, L.labelFont, ImVec2(lp.x, lp.y), ToneColor(v.tone, a), label);

    // Row 2: location / maneuver / failure, left, truncated with "..." (never
    // shrunk). Same face as row 1 (Bahnschrift: heavier, crisper at small
    // sizes than the Segoe UI body face), off-white for contrast on navy.
    char detail[96];
    const float detailFont = L.detailFont;
    FitText(v.detail, L.row2.Width() - L.padLeft - L.padRight, [&](const char* t, size_t n) {
        return fh->CalcTextSizeA(detailFont, FLT_MAX, 0.0f, t, t + n).x;
    }, detail, sizeof(detail));
    const ImVec2 detailSize = fh->CalcTextSizeA(L.detailFont, FLT_MAX, 0.0f, detail);
    const TextPos dp = AlignLeft(L.row2, L.padLeft, detailSize.x, detailSize.y);
    dl->AddText(fh, L.detailFont, ImVec2(dp.x, dp.y), Col(226, 232, 238, a), detail);
}

void Reset() {
    g_model.Reset();
    g_playerKnown = false;
    g_seat = Seat::OnFoot;
    g_drawnSequence = 0;
}

} // namespace skr::gps::panel
