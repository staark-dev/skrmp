// PHONE-001 — SKR Street Phone: native ImGui draw-list renderer (Concept B).
//
// Draw order (one pass into the caller's draw list):
//   shadow → side buttons → body + rim → bezel
//   [clip: screen] pages (with transition transforms) → call overlay → banner
//                  → status bar → gesture indicator
//   corner masks (round the screen) → camera island → screen edge
// The whole device is faded by scaling vertex alpha after recording.
//
// Pages never push their own clip rects: list rows scroll underneath an opaque
// header band drawn afterwards. That keeps the single page clip valid when a
// page is scaled/translated during a transition (vertices are transformed
// after recording; the clip is set to the transformed page rectangle).

#include "phone/phone_renderer.h"
#include "protocol/phone_protocol.h"
#include "phone/phone_text.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace skr::phone {
namespace {

constexpr float kPi = 3.14159265f;

ImVec2 V(float x, float y) { return ImVec2(x, y); }
constexpr ImU32 Rgba(int r, int g, int b, int a = 255) {
    return (static_cast<ImU32>(a) << IM_COL32_A_SHIFT) | (static_cast<ImU32>(b) << IM_COL32_B_SHIFT) |
           (static_cast<ImU32>(g) << IM_COL32_G_SHIFT) | (static_cast<ImU32>(r) << IM_COL32_R_SHIFT);
}
ImU32 WithAlpha(ImU32 c, float a) {
    const float al = static_cast<float>((c >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(a, 0.0f, 1.0f);
    return (c & ~IM_COL32_A_MASK) | (static_cast<ImU32>(al + 0.5f) << IM_COL32_A_SHIFT);
}
ImU32 Mix(ImU32 a, ImU32 b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    ImU32 out = 0;
    for (int s = 0; s < 32; s += 8) {
        const float ca = static_cast<float>((a >> s) & 0xFF), cb = static_cast<float>((b >> s) & 0xFF);
        out |= static_cast<ImU32>(ca + (cb - ca) * t + 0.5f) << s;
    }
    return out;
}

// ── Palette: SKR Street ────────────────────────────────────────────────
constexpr ImU32 kBodyTop = Rgba(42, 50, 65);
constexpr ImU32 kBodyBottom = Rgba(13, 17, 25);
constexpr ImU32 kRimLight = Rgba(166, 181, 204, 126);
constexpr ImU32 kRimDark = Rgba(40, 48, 62, 90);
constexpr ImU32 kBezel = Rgba(3, 5, 8);
constexpr ImU32 kLime = Rgba(173, 226, 96);
constexpr ImU32 kLimeDeep = Rgba(88, 143, 52);
constexpr ImU32 kCyan = Rgba(63, 208, 224);
constexpr ImU32 kCoral = Rgba(236, 84, 98);
constexpr ImU32 kAmber = Rgba(242, 178, 64);
constexpr ImU32 kText = Rgba(244, 247, 250);
constexpr ImU32 kText2 = Rgba(192, 202, 216);
constexpr ImU32 kText3 = Rgba(152, 164, 180);
constexpr ImU32 kPage = Rgba(8, 12, 19);
constexpr ImU32 kPageTop = Rgba(16, 22, 33);
constexpr ImU32 kSurface = Rgba(19, 27, 39);
constexpr ImU32 kSurfaceHi = Rgba(29, 40, 56);
constexpr ImU32 kHair = Rgba(255, 255, 255, 20);

constexpr ImU32 kHues[8] = {
    Rgba(120, 196, 72), Rgba(48, 176, 198), Rgba(226, 152, 52), Rgba(222, 82, 98),
    Rgba(134, 108, 230), Rgba(36, 172, 146), Rgba(72, 132, 226), Rgba(186, 154, 104),
};

struct AppDesc {
    AppId id;
    const char* label;
    ImU32 top, bottom;
};
constexpr AppDesc kApps[] = {
    {AppId::Home, "Home", 0, 0},
    {AppId::Phone, "Phone", Rgba(150, 222, 86), Rgba(76, 160, 46)},
    {AppId::Messages, "Messages", Rgba(70, 214, 230), Rgba(24, 140, 176)},
    {AppId::Contacts, "Contacts", Rgba(132, 146, 170), Rgba(74, 86, 108)},
    {AppId::Gps, "GPS", Rgba(36, 48, 68), Rgba(14, 20, 31)},
    {AppId::Settings, "Settings", Rgba(104, 112, 126), Rgba(52, 58, 70)},
};
const AppDesc& Desc(AppId a) { return kApps[std::min<size_t>(static_cast<size_t>(a), 5)]; }

// ── Context ────────────────────────────────────────────────────────────
struct Ctx {
    ImDrawList* dl;
    const Layout& L;
    const Controller& c;
    const Model& m;
    Views& v;
    const Fonts* faces;
    ImFont* fh;          // heading family key (its largest face)
    ImFont* fb;          // body family key
    const Pointer& p;
    uint64_t now;
    FrameHits* hits;
    bool interactive;
    float u;
};

bool Hot(Ctx& c, const Rect& r, Action a, uint8_t idx) {
    if (!c.interactive) return false;
    if (c.hits && c.hits->count < FrameHits::kMax) c.hits->items[c.hits->count++] = {r, a, idx};
    return r.Contains(c.p.x, c.p.y) && c.L.screen.Contains(c.p.x, c.p.y);
}
float Pressed(Ctx& c, Action a, uint8_t idx) { return c.c.PressAmount(HitId(a, idx), c.now); }

// ── Text ───────────────────────────────────────────────────────────────
// family is c.fh (heading) or c.fb (body); returns the face to draw `size` with.
ImFont* Pick(const Ctx& c, ImFont* family, float size) {
    ImFont* const* faces = family == c.fh ? c.faces->heading : c.faces->body;
    ImFont* best = family;
    for (int i = 2; i >= 0; --i)
        if (faces[i] && faces[i]->FontSize >= size * 0.85f) best = faces[i];
    return best;
}

ImVec2 Measure(const Ctx& c, ImFont* family, float size, const char* s) {
    return Pick(c, family, size)->CalcTextSizeA(size, FLT_MAX, 0.0f, s);
}

// align: 0 left, 0.5 centre, 1 right. y is the vertical centre of the line.
void Text(Ctx& c, ImFont* family, float size, float x, float cy, ImU32 col, const char* s, float align = 0.0f) {
    if (!s || !*s) return;
    ImFont* f = Pick(c, family, size);
    if (align != 0.0f) x -= f->CalcTextSizeA(size, FLT_MAX, 0.0f, s).x * align;
    c.dl->AddText(f, size, V(std::round(x), std::round(cy - size * 0.52f)), col, s);
}

// Fit s into maxW, cutting at a UTF-8 boundary and appending "...".
const char* Fit(const Ctx& c, ImFont* family, float size, const char* s, float maxW, char* buf, size_t n) {
    ImFont* f = Pick(c, family, size);
    auto Measure = [&](const char* t) { return f->CalcTextSizeA(size, FLT_MAX, 0.0f, t); };
    if (Measure(s).x <= maxW) return s;
    size_t len = (std::min)(std::strlen(s), n - 4);
    while (len > 0) {
        while (len > 0 && (static_cast<unsigned char>(s[len]) & 0xC0) == 0x80) --len;
        std::memcpy(buf, s, len);
        while (len > 0 && buf[len - 1] == ' ') --len;
        std::memcpy(buf + len, "...", 4);
        if (Measure(buf).x <= maxW) return buf;
        --len;
    }
    return "";
}

void Initials(const char* name, char out[3]) {
    out[0] = out[1] = out[2] = 0;
    int k = 0;
    bool start = true;
    for (const char* p = name; *p && k < 2; ++p) {
        if (*p == ' ' || *p == '_' || *p == '.') { start = true; continue; }
        if (start) {
            char ch = *p;
            if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
            out[k++] = ch;
            start = false;
        }
    }
}

// "HH:MM" strings; anything unparsable counts as a gap so its label is shown.
int MinutesBetween(const char* a, const char* b) {
    int ha = 0, ma = 0, hb = 0, mb = 0;
    if (std::sscanf(a, "%d:%d", &ha, &ma) != 2 || std::sscanf(b, "%d:%d", &hb, &mb) != 2) return 1 << 20;
    return std::abs((hb * 60 + mb) - (ha * 60 + ma));
}

void FormatDistance(float m, char* buf, size_t n) {
    if (m < 1000.0f) std::snprintf(buf, n, "%.0f m", std::round(m / 10.0f) * 10.0f);
    else std::snprintf(buf, n, "%.1f km", m / 1000.0f);
}

// ── Geometry helpers ───────────────────────────────────────────────────
void Transform(ImDrawList* dl, int start, ImVec2 pivot, float s, ImVec2 off, float alpha) {
    for (int i = start; i < dl->VtxBuffer.Size; ++i) {
        ImDrawVert& vx = dl->VtxBuffer.Data[i];
        vx.pos.x = pivot.x + (vx.pos.x - pivot.x) * s + off.x;
        vx.pos.y = pivot.y + (vx.pos.y - pivot.y) * s + off.y;
        if (alpha < 0.999f) vx.col = WithAlpha(vx.col, alpha);
    }
}

void Gradient(ImDrawList* dl, int start, const Rect& r, ImU32 top, ImU32 bottom) {
    ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, start, dl->VtxBuffer.Size, V(r.x0, r.y0), V(r.x0, r.y1), top, bottom);
}

void RoundRect(ImDrawList* dl, const Rect& r, ImU32 col, float rad, ImDrawFlags fl = 0) {
    dl->AddRectFilled(V(r.x0, r.y0), V(r.x1, r.y1), col, rad, fl);
}

void SoftShadow(ImDrawList* dl, const Rect& r, float rad, float spread, ImU32 col) {
    constexpr int kLayers = 9;
    for (int i = kLayers; i >= 1; --i) {
        const float t = static_cast<float>(i) / kLayers;
        const float e = spread * t;
        const float a = (1.0f - t) * (1.0f - t) * 0.42f;
        dl->AddRectFilled(V(r.x0 - e, r.y0 - e), V(r.x1 + e, r.y1 + e), WithAlpha(col, a), rad + e);
    }
}

void Glass(Ctx& c, const Rect& r, float rad, float hover = 0) {
    RoundRect(c.dl, r, Mix(Rgba(255, 255, 255, 16), Rgba(255, 255, 255, 30), hover), rad);
    c.dl->AddRect(V(r.x0, r.y0), V(r.x1, r.y1), Rgba(255, 255, 255, 24), rad, 0, 1.0f);
}

// Shared M6 card treatment. It stays in the draw-list renderer so screens do
// not mutate global ImGui style and the frame path performs no allocations.
void SurfaceCard(Ctx& c, const Rect& r, float rad, float hover = 0.0f, ImU32 accent = 0) {
    SoftShadow(c.dl, r.Offset(0, 2.0f * c.u), rad, 5.0f * c.u, Rgba(0, 0, 0, 90));
    const int v0 = c.dl->VtxBuffer.Size;
    RoundRect(c.dl, r, Mix(kSurface, kSurfaceHi, 0.18f + hover * 0.16f), rad);
    Gradient(c.dl, v0, r, Mix(kSurfaceHi, Rgba(42, 55, 74), hover * 0.12f), kSurface);
    c.dl->AddRect(V(r.x0, r.y0), V(r.x1, r.y1), Rgba(255, 255, 255, 22 + static_cast<int>(hover * 18)), rad, 0, 1.0f);
    if (accent) RoundRect(c.dl, {r.x0, r.y0 + rad * 0.55f, r.x0 + 2.5f * c.u, r.y1 - rad * 0.55f}, accent, 1.3f * c.u);
}

// ── Glyphs (vector, unit = s) ──────────────────────────────────────────
ImVec2 Rot(ImVec2 c, float x, float y, float a) {
    const float cs = std::cos(a), sn = std::sin(a);
    return V(c.x + x * cs - y * sn, c.y + x * sn + y * cs);
}

void GlyphHandset(ImDrawList* dl, ImVec2 c, float s, ImU32 col, float angle = 0) {
    const ImVec2 o = Rot(c, 0.17f * s, -0.17f * s, angle);
    const float r = 0.40f * s, a0 = (70.0f / 180.0f) * kPi + angle, a1 = (200.0f / 180.0f) * kPi + angle;
    dl->PathArcTo(o, r, a0, a1, 14);
    dl->PathStroke(col, 0, 0.17f * s);
    const float er = 0.115f * s;
    dl->AddCircleFilled(V(o.x + std::cos(a0) * (r - 0.02f * s), o.y + std::sin(a0) * (r - 0.02f * s)), er, col, 10);
    dl->AddCircleFilled(V(o.x + std::cos(a1) * (r - 0.02f * s), o.y + std::sin(a1) * (r - 0.02f * s)), er, col, 10);
}

void GlyphPerson(ImDrawList* dl, ImVec2 c, float s, ImU32 col) {
    dl->AddCircleFilled(V(c.x, c.y - 0.15f * s), 0.15f * s, col, 16);
    dl->PathArcTo(V(c.x, c.y + 0.34f * s), 0.29f * s, kPi, 2.0f * kPi, 16);
    dl->PathFillConvex(col);
}

void GlyphBubble(ImDrawList* dl, ImVec2 c, float s, ImU32 col, ImU32 cut) {
    dl->AddRectFilled(V(c.x - 0.34f * s, c.y - 0.27f * s), V(c.x + 0.34f * s, c.y + 0.17f * s), col, 0.13f * s);
    dl->AddTriangleFilled(V(c.x - 0.22f * s, c.y + 0.12f * s), V(c.x - 0.04f * s, c.y + 0.15f * s),
                          V(c.x - 0.26f * s, c.y + 0.32f * s), col);
    for (int i = -1; i <= 1; ++i) dl->AddCircleFilled(V(c.x + i * 0.15f * s, c.y - 0.05f * s), 0.045f * s, cut, 8);
}

void GlyphNav(ImDrawList* dl, ImVec2 c, float s, ImU32 col, float angle = 0) {
    const ImVec2 pts[4] = {Rot(c, 0, -0.36f * s, angle), Rot(c, 0.27f * s, 0.30f * s, angle),
                           Rot(c, 0, 0.16f * s, angle), Rot(c, -0.27f * s, 0.30f * s, angle)};
    dl->AddConcavePolyFilled(pts, 4, col);
}

void GlyphGear(ImDrawList* dl, ImVec2 c, float s, ImU32 col, ImU32 cut) {
    for (int i = 0; i < 8; ++i) {
        const float a = i * kPi / 4.0f;
        const ImVec2 q[4] = {Rot(c, -0.07f * s, -0.37f * s, a), Rot(c, 0.07f * s, -0.37f * s, a),
                             Rot(c, 0.09f * s, -0.20f * s, a), Rot(c, -0.09f * s, -0.20f * s, a)};
        dl->AddConvexPolyFilled(q, 4, col);
    }
    dl->AddCircleFilled(c, 0.26f * s, col, 24);
    dl->AddCircleFilled(c, 0.105f * s, cut, 16);
}

void GlyphPin(ImDrawList* dl, ImVec2 c, float s, ImU32 col, ImU32 cut) {
    const ImVec2 h(c.x, c.y - 0.10f * s);
    dl->AddCircleFilled(h, 0.24f * s, col, 18);
    dl->AddTriangleFilled(V(h.x - 0.21f * s, h.y + 0.11f * s), V(h.x + 0.21f * s, h.y + 0.11f * s),
                          V(c.x, c.y + 0.40f * s), col);
    dl->AddCircleFilled(h, 0.09f * s, cut, 12);
}

void GlyphChevron(ImDrawList* dl, ImVec2 c, float s, ImU32 col, bool left, float th) {
    const float d = left ? 1.0f : -1.0f;
    dl->PathLineTo(V(c.x + 0.16f * s * d, c.y - 0.30f * s));
    dl->PathLineTo(V(c.x - 0.14f * s * d, c.y));
    dl->PathLineTo(V(c.x + 0.16f * s * d, c.y + 0.30f * s));
    dl->PathStroke(col, 0, th);
}

void GlyphSearch(ImDrawList* dl, ImVec2 c, float s, ImU32 col, float th) {
    dl->AddCircle(V(c.x - 0.06f * s, c.y - 0.06f * s), 0.24f * s, col, 16, th);
    dl->AddLine(V(c.x + 0.12f * s, c.y + 0.12f * s), V(c.x + 0.32f * s, c.y + 0.32f * s), col, th * 1.2f);
}

void GlyphArrowUp(ImDrawList* dl, ImVec2 c, float s, ImU32 col, float th) {
    dl->AddLine(V(c.x, c.y + 0.28f * s), V(c.x, c.y - 0.26f * s), col, th);
    dl->PathLineTo(V(c.x - 0.22f * s, c.y - 0.04f * s));
    dl->PathLineTo(V(c.x, c.y - 0.28f * s));
    dl->PathLineTo(V(c.x + 0.22f * s, c.y - 0.04f * s));
    dl->PathStroke(col, 0, th);
}

void GlyphPencil(ImDrawList* dl, ImVec2 c, float s, ImU32 col, float th) {
    dl->AddRect(V(c.x - 0.32f * s, c.y - 0.26f * s), V(c.x + 0.24f * s, c.y + 0.32f * s), col, 0.08f * s, 0, th);
    dl->AddLine(V(c.x - 0.05f * s, c.y + 0.06f * s), V(c.x + 0.34f * s, c.y - 0.34f * s), col, th * 1.3f);
}

void GlyphCamera(ImDrawList* dl, ImVec2 c, float s, ImU32 col, ImU32 cut) {
    dl->AddRectFilled(V(c.x - 0.36f * s, c.y - 0.24f * s), V(c.x + 0.36f * s, c.y + 0.27f * s), col, 0.10f * s);
    dl->AddRectFilled(V(c.x - 0.18f * s, c.y - 0.34f * s), V(c.x + 0.10f * s, c.y - 0.18f * s), col, 0.06f * s);
    dl->AddCircleFilled(c, 0.17f * s, cut, 16);
    dl->AddCircle(c, 0.10f * s, col, 16, 0.05f * s);
}

void GlyphApps(ImDrawList* dl, ImVec2 c, float s, ImU32 col) {
    const float q = 0.13f * s, gap = 0.12f * s;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            dl->AddRectFilled(V(c.x + x * (q * 2 + gap) - q, c.y + y * (q * 2 + gap) - q),
                              V(c.x + x * (q * 2 + gap) + q, c.y + y * (q * 2 + gap) + q), col, 0.05f * s);
}

void GlyphWallet(ImDrawList* dl, ImVec2 c, float s, ImU32 col, ImU32 cut) {
    dl->AddRectFilled(V(c.x - 0.38f * s, c.y - 0.25f * s), V(c.x + 0.38f * s, c.y + 0.27f * s), col, 0.10f * s);
    dl->AddRectFilled(V(c.x + 0.02f * s, c.y - 0.10f * s), V(c.x + 0.42f * s, c.y + 0.14f * s), cut, 0.07f * s);
    dl->AddCircleFilled(V(c.x + 0.26f * s, c.y + 0.02f * s), 0.045f * s, col, 8);
}

void Avatar(Ctx& c, ImVec2 ctr, float r, uint8_t hue, const char* name) {
    const ImU32 base = kHues[hue & 7];
    const int v0 = c.dl->VtxBuffer.Size;
    c.dl->AddCircleFilled(ctr, r, base, 28);
    ImGui::ShadeVertsLinearColorGradientKeepAlpha(c.dl, v0, c.dl->VtxBuffer.Size, V(ctr.x, ctr.y - r), V(ctr.x, ctr.y + r),
                                                  Mix(base, Rgba(255, 255, 255), 0.18f), Mix(base, Rgba(0, 0, 0), 0.28f));
    char ini[3];
    Initials(name, ini);
    Text(c, c.fh, r * 0.92f, ctr.x, ctr.y, Rgba(255, 255, 255, 235), ini, 0.5f);
}

// ── App icon (home grid, banner) ───────────────────────────────────────
void AppGlyph(ImDrawList* dl, AppId app, ImVec2 c, float s, ImU32 bg) {
    const ImU32 w = Rgba(255, 255, 255, 245);
    switch (app) {
        case AppId::Phone: GlyphHandset(dl, c, s * 0.92f, w); break;
        case AppId::Messages: GlyphBubble(dl, c, s, w, bg); break;
        case AppId::Contacts: GlyphPerson(dl, c, s, w); break;
        case AppId::Gps: GlyphNav(dl, c, s * 0.95f, kLime, 0.32f); break;
        case AppId::Settings: GlyphGear(dl, c, s, w, bg); break;
        default: break;
    }
}

void AppIcon(Ctx& c, AppId app, const Rect& r, float hover, float press) {
    const AppDesc& d = Desc(app);
    const float k = 1.0f - 0.08f * press;
    const float hw = r.W() * 0.5f * k, hh = r.H() * 0.5f * k;
    const Rect q{r.Cx() - hw, r.Cy() - hh, r.Cx() + hw, r.Cy() + hh};
    const float rad = q.W() * 0.27f;
    SoftShadow(c.dl, q.Offset(0, 2.0f * c.u), rad, 5.0f * c.u, Rgba(0, 0, 0, 120));
    const int v0 = c.dl->VtxBuffer.Size;
    RoundRect(c.dl, q, d.top, rad);
    Gradient(c.dl, v0, q, Mix(d.top, Rgba(255, 255, 255), 0.10f * hover), d.bottom);
    c.dl->AddRect(V(q.x0, q.y0), V(q.x1, q.y1), Rgba(255, 255, 255, 30 + static_cast<int>(40 * hover)), rad, 0, 1.0f);
    AppGlyph(c.dl, app, V(q.Cx(), q.Cy()), q.W() * 0.62f, Mix(d.top, d.bottom, 0.5f));
    if (press > 0) RoundRect(c.dl, q, Rgba(0, 0, 0, static_cast<int>(60 * press)), rad);
}

void FutureAppIcon(Ctx& c, const Rect& r, const char* label, int glyph, ImU32 top, ImU32 bottom) {
    const float u = c.u, rad = r.W() * 0.27f;
    SoftShadow(c.dl, r.Offset(0, 1.5f * u), rad, 4.0f * u, Rgba(0, 0, 0, 90));
    const int v0 = c.dl->VtxBuffer.Size;
    RoundRect(c.dl, r, top, rad);
    Gradient(c.dl, v0, r, top, bottom);
    c.dl->AddRect(V(r.x0, r.y0), V(r.x1, r.y1), Rgba(255, 255, 255, 34), rad, 0, 1.0f);
    const ImVec2 center(r.Cx(), r.Cy() - 1.0f * u);
    const float size = r.W() * 0.58f;
    if (glyph == 0) GlyphCamera(c.dl, center, size, Rgba(246, 248, 252), Mix(top, bottom, 0.5f));
    else if (glyph == 1) GlyphApps(c.dl, center, size, Rgba(220, 226, 234));
    else GlyphWallet(c.dl, center, size, Rgba(246, 248, 252), Mix(top, bottom, 0.5f));
    const ImVec2 labelSize = Measure(c, c.fh, 8.5f * u, label);
    const Rect labelBg{r.Cx() - labelSize.x * 0.5f - 3.0f * u, r.y1 + 3.5f * u,
                       r.Cx() + labelSize.x * 0.5f + 3.0f * u, r.y1 + 15.5f * u};
    RoundRect(c.dl, labelBg, Rgba(3, 7, 12, 150), labelBg.H() * 0.5f);
    Text(c, c.fh, 8.5f * u, r.Cx(), r.y1 + 9.5f * u, kText, label, 0.5f);
}

// ── Page chrome ────────────────────────────────────────────────────────
void PageBackground(Ctx& c, const Rect& P) {
    const int v0 = c.dl->VtxBuffer.Size;
    RoundRect(c.dl, P, kPage, c.L.screenRadius);
    Gradient(c.dl, v0, {P.x0, P.y0, P.x1, P.y0 + P.H() * 0.45f}, kPageTop, kPage);
}

// Opaque header band; returns its bottom edge.
float Header(Ctx& c, const Rect& P, const char* title, bool back, float height, const char* right = nullptr) {
    const float u = c.u;
    const Rect band{P.x0, P.y0, P.x1, c.L.content.y0 + height};
    const int v0 = c.dl->VtxBuffer.Size;
    RoundRect(c.dl, band, kPageTop, c.L.screenRadius, ImDrawFlags_RoundCornersTop);
    Gradient(c.dl, v0, band, kPageTop, Mix(kPageTop, kPage, 0.6f));
    c.dl->AddLine(V(band.x0, band.y1 - 0.5f), V(band.x1, band.y1 - 0.5f), kHair, 1.0f);
    const float cy = c.L.content.y0 + 19.0f * u;
    float tx = P.x0 + 15.0f * u;
    if (back) {
        const Rect hit{P.x0 + 4.0f * u, cy - 15.0f * u, P.x0 + 34.0f * u, cy + 15.0f * u};
        const bool hov = Hot(c, hit, Action::Back, 0);
        const float pr = Pressed(c, Action::Back, 0);
        if (hov || pr > 0) c.dl->AddCircleFilled(V(hit.Cx(), cy), 13.0f * u, Rgba(255, 255, 255, 18 + static_cast<int>(30 * pr)), 20);
        GlyphChevron(c.dl, V(hit.Cx(), cy), 22.0f * u, hov ? kLime : kText, true, 2.2f * u);
        tx = P.x0 + 36.0f * u;
    }
    if (title) Text(c, c.fh, c.L.fsTitle, tx, cy, kText, title);
    if (right) Text(c, c.fb, c.L.fsSmall, P.x1 - 15.0f * u, cy, kText3, right, 1.0f);
    return band.y1;
}

void Scrollbar(Ctx& c, const Rect& P, float top, float bottom, float total, float scroll) {
    const float vis = bottom - top;
    if (total <= vis + 1) return;
    const float u = c.u;
    const float h = (std::max)(18.0f * u, vis * vis / total);
    const float y = top + (vis - h) * std::clamp(scroll / (total - vis), 0.0f, 1.0f);
    RoundRect(c.dl, {P.x1 - 4.5f * u, y, P.x1 - 2.5f * u, y + h}, Rgba(255, 255, 255, 60), 1.0f * u);
}

// ── Wallpaper ──────────────────────────────────────────────────────────
// Layered, procedural (no texture to load, lose on D3D9 Reset or stretch):
//   sky gradient → sun haze → skyline + palms (bottom 24 %) → ground fade →
//   readability scrim under the app grid.
// The skyline sits BELOW the clock/GPS widgets and fades out downwards
// instead of ending in a hard ground band, so no icon row ever straddles a
// silhouette edge. Everything scales with the screen rect (no stretching:
// building widths are fractions of W, heights fractions of W too).
// Image wallpaper (assets/bg.bmp, see phone_wallpaper.h). Set by the loader.
void* g_wallTex = nullptr;
float g_wallW = 0, g_wallH = 0;
// Horizontal focus of the crop: the sun / tall palm column of bg.bmp.
constexpr float kWallFocusX = 0.44f;

// Vertical alpha ramp over [y0, y1] inside the rounded screen P. Gradient()
// keeps vertex alpha, so ramps use multi-colour rects; the parts inside the
// corner radius are rounded strips at the end alpha.
void AlphaRamp(ImDrawList* dl, const Rect& P, float rad, float y0, float y1, int a0, int a1) {
    const ImU32 c0 = Rgba(0, 0, 0, a0), c1 = Rgba(0, 0, 0, a1);
    float top = y0, bottom = y1;
    if (y0 <= P.y0 + 0.5f) {
        dl->AddRectFilled(V(P.x0, P.y0), V(P.x1, P.y0 + rad), c0, rad, ImDrawFlags_RoundCornersTop);
        top = P.y0 + rad;
    }
    if (y1 >= P.y1 - 0.5f) {
        dl->AddRectFilled(V(P.x0, P.y1 - rad), V(P.x1, P.y1), c1, rad, ImDrawFlags_RoundCornersBottom);
        bottom = P.y1 - rad;
    }
    if (bottom > top) {
        const float t0 = (top - y0) / (std::max)(1.0f, y1 - y0), t1 = (bottom - y0) / (std::max)(1.0f, y1 - y0);
        const ImU32 ct = Mix(c0, c1, t0), cb = Mix(c0, c1, t1);
        dl->AddRectFilledMultiColor(V(P.x0, top), V(P.x1, bottom), ct, ct, cb, cb);
    }
}

void WallpaperImage(Ctx& c, const Rect& P) {
    ImDrawList* dl = c.dl;
    const float rad = c.L.screenRadius;
    // Cover-fit: crop the overflowing axis, never stretch.
    const float ia = g_wallW / g_wallH, sa = P.W() / P.H();
    ImVec2 uv0(0, 0), uv1(1, 1);
    if (ia > sa) {
        const float k = sa / ia;
        const float x0 = std::clamp(kWallFocusX - k * 0.5f, 0.0f, 1.0f - k);
        uv0.x = x0; uv1.x = x0 + k;
    } else {
        const float k = ia / sa;
        uv0.y = (1.0f - k) * 0.5f; uv1.y = uv0.y + k;
    }
    dl->AddImageRounded(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(g_wallTex)), V(P.x0, P.y0), V(P.x1, P.y1),
                        uv0, uv1, Rgba(255, 255, 255), rad);
    // The art is light (paper sky): dim it a little, then darken the clock
    // band and the app grid so white text keeps its contrast.
    RoundRect(dl, P, Rgba(0, 0, 0, 55), rad);
    AlphaRamp(dl, P, rad, P.y0, P.y0 + P.H() * 0.46f, 170, 0);
    const float gridTop = AppIconRect(c.L, AppId::Phone).y0 - 22.0f * c.u;
    AlphaRamp(dl, P, rad, gridTop, P.y1, 0, 175);
}

void Wallpaper(Ctx& c, const Rect& P) {
    if (c.m.profile.wallpaper==0 && g_wallTex && g_wallW > 0 && g_wallH > 0) { WallpaperImage(c, P); return; }
    ImDrawList* dl = c.dl;
    const float W = P.W(), H = P.H();
    const float rad = c.L.screenRadius;

    int v0 = dl->VtxBuffer.Size;
    RoundRect(dl, P, Rgba(10, 15, 30), rad);
    Gradient(dl, v0, {P.x0, P.y0, P.x1, P.y0 + H * 0.62f}, Rgba(9, 13, 28), Rgba(17, 52, 66));
    // Lower half: dusk glow, brightest at the horizon so the silhouettes read.
    const float base = P.y0 + H * 0.93f;
    v0 = dl->VtxBuffer.Size;
    dl->AddRectFilled(V(P.x0, P.y0 + H * 0.62f), V(P.x1, P.y1), Rgba(17, 52, 66), rad, ImDrawFlags_RoundCornersBottom);
    Gradient(dl, v0, {P.x0, P.y0 + H * 0.62f, P.x1, base}, Rgba(17, 52, 66), Rgba(34, 92, 96));

    // Low sun on the horizon, right of the grid's second row.
    const ImVec2 sun(P.x0 + W * 0.80f, base - 0.07f * W);
    for (int i = 6; i >= 1; --i)
        dl->AddCircleFilled(sun, W * (0.08f + 0.06f * i), WithAlpha(Mix(kLime, kCyan, i / 6.0f), 0.05f), 40);
    dl->AddCircleFilled(sun, W * 0.085f, WithAlpha(kLime, 0.5f), 40);

    // Skyline silhouette (deterministic), standing on the baseline.
    static const float kW[] = {0.07f, 0.10f, 0.06f, 0.09f, 0.05f, 0.11f, 0.07f, 0.08f, 0.06f, 0.10f, 0.07f, 0.09f, 0.08f};
    static const float kH[] = {0.10f, 0.18f, 0.13f, 0.26f, 0.09f, 0.21f, 0.15f, 0.31f, 0.12f, 0.19f, 0.10f, 0.16f, 0.12f};
    const ImU32 sil = Rgba(6, 11, 18, 235);
    float x = P.x0 - W * 0.02f;
    for (int i = 0; i < 13 && x < P.x1; ++i) {
        const float bw = kW[i] * W, bh = kH[i] * W;
        dl->AddRectFilled(V(x, base - bh), V(x + bw - 1.0f, base + 1.0f), sil);
        if (i == 7) dl->AddRectFilled(V(x + bw * 0.45f, base - bh - 0.05f * W), V(x + bw * 0.55f, base - bh), sil);
        const float wsz = (std::max)(1.0f, 1.4f * c.u);
        for (int row = 0; row < 10; ++row)
            for (int col = 0; col < 4; ++col) {
                const float wx = x + bw * (0.18f + 0.2f * col), wy = base - bh + (6.0f + 7.0f * row) * c.u;
                if (wy > base - 4.0f * c.u || wx + wsz > x + bw - 2) continue;
                const unsigned hsh = static_cast<unsigned>(i * 131 + row * 17 + col * 7) * 2654435761u;
                if ((hsh >> 27) % 7 != 0) continue;
                dl->AddRectFilled(V(wx, wy), V(wx + wsz, wy + wsz), (hsh >> 24) & 1 ? WithAlpha(kLime, 0.45f) : WithAlpha(kCyan, 0.4f));
            }
        x += bw;
    }
    // Ground: fades from the silhouette colour into the bottom of the screen.
    v0 = dl->VtxBuffer.Size;
    dl->AddRectFilled(V(P.x0, base), V(P.x1, P.y1), sil, rad, ImDrawFlags_RoundCornersBottom);
    Gradient(dl, v0, {P.x0, base, P.x1, P.y1}, sil, Rgba(4, 7, 12));
    dl->AddLine(V(P.x0, base + 0.5f), V(P.x1, base + 0.5f), WithAlpha(kLime, 0.28f), 1.0f);

    // Two palms.
    auto palm = [&](float px, float h, float lean) {
        const ImVec2 b(px, base + 2.0f), t(px + lean * W, base - h);
        dl->AddBezierQuadratic(b, V(px + lean * W * 0.2f, base - h * 0.55f), t, sil, 2.4f * c.u, 12);
        static const float kFr[][2] = {{-0.13f, 0.05f}, {-0.09f, -0.03f}, {0.0f, -0.06f}, {0.09f, -0.03f}, {0.13f, 0.05f}, {0.05f, 0.08f}, {-0.05f, 0.08f}};
        for (const auto& f : kFr) {
            const ImVec2 e(t.x + f[0] * W, t.y + f[1] * W + 0.04f * W);
            dl->AddBezierQuadratic(t, V((t.x + e.x) * 0.5f, (std::min)(t.y, e.y) - 0.035f * W), e, sil, 2.2f * c.u, 10);
        }
    };
    palm(P.x0 + W * 0.13f, W * 0.52f, 0.03f);
    palm(P.x0 + W * 0.90f, W * 0.44f, -0.035f);

    // Readability scrim behind the app grid (labels sit over the skyline).
    // Alpha ramps, so it cannot use Gradient (which keeps vertex alpha); the
    // last `rad` px are a rounded strip at the final alpha.
    const float scrimTop = AppIconRect(c.L, AppId::Phone).y0 - 18.0f * c.u;
    const float scrimEnd = P.y1 - rad;
    if (scrimEnd > scrimTop) {
        const ImU32 a0 = Rgba(0, 0, 0, 0), a1 = Rgba(0, 0, 0, 110);
        dl->AddRectFilledMultiColor(V(P.x0, scrimTop), V(P.x1, scrimEnd), a0, a0, a1, a1);
        dl->AddRectFilled(V(P.x0, scrimEnd), V(P.x1, P.y1), a1, rad, ImDrawFlags_RoundCornersBottom);
    }
}

// ── Shared widgets (PHONE-M4F) ─────────────────────────────────────────
void Spinner(Ctx& c, ImVec2 ctr, float r, ImU32 col) {
    const float a = static_cast<float>(c.now % 1000) / 1000.0f * 2.0f * kPi;
    c.dl->PathArcTo(ctr, r, a, a + kPi * 1.4f, 20);
    c.dl->PathStroke(col, 0, (std::max)(1.5f, 0.22f * r));
}

// Pill button. Returns hovered.
bool Button(Ctx& c, const Rect& r, const char* label, Action a, uint8_t idx, ImU32 fill, ImU32 ink, bool enabled = true) {
    const bool hov = enabled && Hot(c, r, a, idx);
    const float pr = enabled ? Pressed(c, a, idx) : 0.0f;
    ImU32 bg = enabled ? fill : Mix(fill, kSurface, 0.65f);
    if (hov) bg = Mix(bg, Rgba(255, 255, 255), 0.12f);
    if (pr > 0) bg = Mix(bg, Rgba(0, 0, 0), 0.2f * pr);
    RoundRect(c.dl, r, bg, r.H() * 0.5f);
    Text(c, c.fh, c.L.fsBody * 0.92f, r.Cx(), r.Cy(), enabled ? ink : WithAlpha(ink, 0.45f), label, 0.5f);
    return hov;
}

// Single-line text input. Shows the tail of long text, a blinking caret when
// focused and the placeholder when empty. Tapping it focuses the field.
void InputField(Ctx& c, const Rect& r, const TextField& f, const char* placeholder, Field id, float fs, float padLeftU = 11.0f) {
    const float u = c.u;
    const bool focused = c.v.focus == id;
    const bool hov = Hot(c, r, Action::FocusField, static_cast<uint8_t>(id));
    RoundRect(c.dl, r, Rgba(255, 255, 255, focused ? 30 : hov ? 24 : 16), (std::min)(r.H() * 0.5f, 12.0f * u));
    if (focused)
        c.dl->AddRect(V(r.x0, r.y0), V(r.x1, r.y1), WithAlpha(kLime, 0.75f), (std::min)(r.H() * 0.5f, 12.0f * u), 0, 1.2f * u);
    const float x0 = r.x0 + padLeftU * u, maxW = r.W() - (padLeftU + 11.0f) * u;
    if (!f.len) {
        Text(c, c.fb, fs, x0, r.Cy(), kText3, placeholder);
        if (focused && (c.now / 530) % 2 == 0)
            c.dl->AddLine(V(x0, r.Cy() - fs * 0.6f), V(x0, r.Cy() + fs * 0.6f), kLime, 1.3f * u);
        return;
    }
    // Tail that fits: skip whole UTF-8 sequences from the front.
    ImFont* face = Pick(c, c.fb, fs);
    const char* s = f.text;
    while (*s && face->CalcTextSizeA(fs, FLT_MAX, 0.0f, s).x > maxW - 4.0f * u) {
        ++s;
        while ((static_cast<unsigned char>(*s) & 0xC0) == 0x80) ++s;
    }
    Text(c, c.fb, fs, x0, r.Cy(), kText, s);
    if (focused && (c.now / 530) % 2 == 0) {
        const float cx = x0 + face->CalcTextSizeA(fs, FLT_MAX, 0.0f, s).x + 1.5f * u;
        c.dl->AddLine(V(cx, r.Cy() - fs * 0.6f), V(cx, r.Cy() + fs * 0.6f), kLime, 1.3f * u);
    }
}

// Centred placeholder for empty / loading / offline lists.
void EmptyState(Ctx& c, const Rect& area, const char* title, const char* sub, bool loading, bool retry) {
    const float u = c.u;
    const float cy = area.Cy() - 3.0f * u;
    const ImVec2 badge(area.Cx(), cy - 37.0f * u);
    c.dl->AddCircleFilled(badge, 20.0f * u, Rgba(25, 34, 48), 28);
    c.dl->AddCircle(badge, 20.0f * u, Rgba(255, 255, 255, 24), 28, 1.0f);
    if (loading) Spinner(c, badge, 8.0f * u, kLime);
    else {
        c.dl->AddCircleFilled(badge, 3.0f * u, retry ? kCoral : kText3, 12);
        c.dl->AddCircle(badge, 8.0f * u, WithAlpha(retry ? kCoral : kText3, 0.35f), 18, 1.2f * u);
    }
    Text(c, c.fh, c.L.fsBody, area.Cx(), cy, kText2, title, 0.5f);
    if (sub && *sub) Text(c, c.fb, c.L.fsSmall, area.Cx(), cy + 16.0f * u, kText3, sub, 0.5f);
    if (retry) {
        const Rect b{area.Cx() - 45.0f * u, cy + 30.0f * u, area.Cx() + 45.0f * u, cy + 56.0f * u};
        Button(c, b, "Try again", Action::Retry, 0, kLime, Rgba(14, 26, 8));
    }
}

// What a list page shows when the phone backend is not usable.
bool ServiceUnavailable(Ctx& c, const Rect& area) {
    switch (c.m.sync) {
        case SyncState::Ready: return false;
        case SyncState::Loading: EmptyState(c, area, "Connecting...", "Getting your number", true, false); return true;
        case SyncState::Error: EmptyState(c, area, "Phone unavailable", c.m.syncError, false, true); return true;
        default: EmptyState(c, area, "No service", "This server has no phone network", false, false); return true;
    }
}

// Round icon button in a header (16 u radius hit).
bool HeaderIcon(Ctx& c, ImVec2 ctr, Action a, uint8_t idx) {
    const float u = c.u;
    const Rect hit{ctr.x - 14.0f * u, ctr.y - 14.0f * u, ctr.x + 14.0f * u, ctr.y + 14.0f * u};
    const bool hov = Hot(c, hit, a, idx);
    const float pr = Pressed(c, a, idx);
    if (hov || pr > 0) c.dl->AddCircleFilled(ctr, 13.0f * u, Rgba(255, 255, 255, 18 + static_cast<int>(30 * pr)), 20);
    return hov;
}

void GlyphPlus(ImDrawList* dl, ImVec2 c, float s, ImU32 col, float th) {
    dl->AddLine(V(c.x - 0.32f * s, c.y), V(c.x + 0.32f * s, c.y), col, th);
    dl->AddLine(V(c.x, c.y - 0.32f * s), V(c.x, c.y + 0.32f * s), col, th);
}

// ── Pages ──────────────────────────────────────────────────────────────
void PageHome(Ctx& c, const Rect& P) {
    const float u = c.u;
    const Layout& L = c.L;
    const Rect C{P.x0, L.content.y0, P.x1, L.content.y1};
    Wallpaper(c, P);

    // Clock + date (+ location when known).
    const float clockCy = C.y0 + 38.0f * u;
    Text(c, c.fh, L.fsClock, C.Cx() + 1.0f, clockCy + 1.5f * u, Rgba(0, 0, 0, 90), c.m.status.time, 0.5f);
    Text(c, c.fh, L.fsClock, C.Cx(), clockCy, kText, c.m.status.time, 0.5f);
    Text(c, c.fb, L.fsBody, C.Cx() + 1.0f, C.y0 + 73.0f * u, Rgba(0, 0, 0, 140), c.m.status.date, 0.5f);
    Text(c, c.fb, L.fsBody, C.Cx(), C.y0 + 72.0f * u, Rgba(228, 234, 242, 235), c.m.status.date, 0.5f);
    if (c.m.status.location[0]) {
        char buf[64];
        const char* loc = Fit(c, c.fb, L.fsSmall, c.m.status.location, C.W() - 60.0f * u, buf, sizeof(buf));
        const float tw = Measure(c, c.fb, L.fsSmall, loc).x, gw = 10.0f * u;
        const float x0 = C.Cx() - (tw + gw + 4.0f * u) * 0.5f, cy = C.y0 + 90.0f * u;
        GlyphPin(c.dl, V(x0 + gw * 0.5f, cy), gw * 1.15f, kLime, Rgba(14, 30, 40));
        Text(c, c.fb, L.fsSmall, x0 + gw + 4.0f * u, cy, Rgba(200, 212, 226, 220), loc);
    } else if (c.m.sync == SyncState::Ready) {
        char line[40];
        std::snprintf(line, sizeof(line), "My number  %s", c.m.ownNumber);
        Text(c, c.fb, L.fsSmall, C.Cx() + 1.0f, C.y0 + 91.0f * u, Rgba(0, 0, 0, 150), line, 0.5f);
        Text(c, c.fb, L.fsSmall, C.Cx(), C.y0 + 90.0f * u, Rgba(222, 230, 240, 230), line, 0.5f);
    }

    // GPS widget.
    {
        const Rect card{C.x0 + 12.0f * u, C.y0 + 110.0f * u, C.x1 - 12.0f * u, C.y0 + 156.0f * u};
        const bool hov = Hot(c, card, Action::OpenApp, static_cast<uint8_t>(AppId::Gps));
        const float pr = Pressed(c, Action::OpenApp, static_cast<uint8_t>(AppId::Gps));
        SurfaceCard(c, card, 14.0f * u, hov ? 1.0f : 0.0f, kLime);
        if (pr > 0) RoundRect(c.dl, card, Rgba(0, 0, 0, static_cast<int>(50 * pr)), 14.0f * u);
        const ImVec2 ic(card.x0 + 23.0f * u, card.Cy());
        c.dl->AddCircleFilled(ic, 14.0f * u, Rgba(24, 40, 30), 24);
        c.dl->AddCircle(ic, 14.0f * u, WithAlpha(kLime, 0.5f), 24, 1.0f);
        GlyphNav(c.dl, ic, 17.0f * u, kLime, 0.32f);
        const float tx = card.x0 + 44.0f * u, tw = card.x1 - tx - 10.0f * u;
        char buf[48], dist[24], line[64];
        const GpsInfo& g = c.m.gps;
        if (g.status == NavStatus::Idle) {
            Text(c, c.fh, L.fsBody, tx, card.Cy() - 7.0f * u, kText, "No destination");
            Text(c, c.fb, L.fsSmall, tx, card.Cy() + 8.0f * u, kText2, "Tap to choose a place");
        } else {
            Text(c, c.fh, L.fsBody, tx, card.Cy() - 7.0f * u, kText, Fit(c, c.fh, L.fsBody, g.destName, tw, buf, sizeof(buf)));
            FormatDistance(g.remainingM, dist, sizeof(dist));
            std::snprintf(line, sizeof(line), "%s \xC2\xB7 %s", dist,
                          g.status == NavStatus::Navigating ? "Navigating" : g.status == NavStatus::Calculating ? "Calculating" : "Arrived");
            Text(c, c.fb, L.fsSmall, tx, card.Cy() + 8.0f * u, kLime, line);
        }
    }

    // Familiar three-app dock. Labels are intentionally omitted in the dock;
    // the two apps on the wallpaper keep their labels and generous spacing.
    const Rect dock{C.x0 + 18.0f * u, C.y1 - 49.0f * u, C.x1 - 18.0f * u, C.y1 - 5.0f * u};
    RoundRect(c.dl, dock, Rgba(8, 13, 21, 178), 13.0f * u);
    c.dl->AddRect(V(dock.x0, dock.y0), V(dock.x1, dock.y1), Rgba(255, 255, 255, 24), 13.0f * u, 0, 1.0f);

    // App grid and dock.
    static const AppId kGrid[] = {AppId::Phone, AppId::Messages, AppId::Contacts, AppId::Gps, AppId::Settings};
    for (AppId a : kGrid) {
        const Rect r = AppIconRect(L, a);
        const uint8_t idx = static_cast<uint8_t>(a);
        const bool hov = Hot(c, {r.x0 - 6.0f * u, r.y0 - 4.0f * u, r.x1 + 6.0f * u, r.y1 + 16.0f * u}, Action::OpenApp, idx);
        AppIcon(c, a, r, hov ? 1.0f : 0.0f, Pressed(c, Action::OpenApp, idx));
        const bool inDock = a == AppId::Phone || a == AppId::Messages || a == AppId::Settings;
        if (!inDock) {
            const ImVec2 labelSize = Measure(c, c.fh, L.fsSmall, Desc(a).label);
            const Rect labelBg{r.Cx() - labelSize.x * 0.5f - 4.0f * u, r.y1 + 4.0f * u,
                               r.Cx() + labelSize.x * 0.5f + 4.0f * u, r.y1 + 17.0f * u};
            RoundRect(c.dl, labelBg, Rgba(3, 7, 12, hov ? 190 : 145), labelBg.H() * 0.5f);
            Text(c, c.fh, L.fsSmall, r.Cx(), r.y1 + 10.5f * u, Rgba(248, 250, 252, 255), Desc(a).label, 0.5f);
        }
        int unread = 0;
        if (a == AppId::Messages)
            for (int i = 0; i < c.m.threadCount; ++i) unread += c.m.threads[i].unread;
        if (unread > 0) {
            char n[8];
            std::snprintf(n, sizeof(n), "%d", (std::min)(unread, 99));
            const ImVec2 bc(r.x1 - 2.0f * u, r.y0 + 2.0f * u);
            c.dl->AddCircleFilled(bc, 8.0f * u, kLime, 18);
            c.dl->AddCircle(bc, 8.0f * u, Rgba(10, 16, 26), 18, 1.5f * u);
            Text(c, c.fh, 10.0f * u, bc.x, bc.y, Rgba(14, 26, 8), n, 0.5f);
        }
    }

    // Future milestones are visible but deliberately non-interactive.
    // Three-column single row, positioned above the dock with a safe gap.
    const float futureSize = 27.0f * u;
    const float futurePad = 12.0f * u;
    const float futureColW = (C.W() - 2.0f * futurePad) / 3.0f;
    const float futureTop = C.y0 + 218.0f * u;
    auto futureRect = [&](int col) {
        const float cx = C.x0 + futurePad + futureColW * (static_cast<float>(col) + 0.5f);
        return Rect{Snap(cx - futureSize * 0.5f), Snap(futureTop), Snap(cx + futureSize * 0.5f), Snap(futureTop + futureSize)};
    };
    FutureAppIcon(c, futureRect(0), "Camera", 0, Rgba(115, 101, 222), Rgba(61, 75, 169));
    FutureAppIcon(c, futureRect(1), "App Store", 1, Rgba(54, 195, 221), Rgba(35, 119, 190));
    const Rect wallet = futureRect(2);
    FutureAppIcon(c, wallet, "Wallet", 2, Rgba(224, 169, 73), Rgba(163, 96, 48));
}

// Phone: the server-assigned number. Calls are not offered: the server has no
// call service yet, so no dialer is shown.
void PagePhone(Ctx& c, const Rect& P) {
    char displayNumber[16]; protocol::phone::FormatPhoneNumber(c.m.ownNumber,displayNumber);
    const float u = c.u;
    const Layout& L = c.L;
    PageBackground(c, P);
    const float hb = Header(c, P, "My Number", true, 38.0f * u);
    const Rect area{P.x0, hb, P.x1, L.content.y1};
    if (ServiceUnavailable(c, area)) return;

    const Rect card{P.x0 + 12.0f * u, hb + 18.0f * u, P.x1 - 12.0f * u, hb + 150.0f * u};
    SurfaceCard(c, card, 16.0f * u, 0.0f, kLime);

    const ImVec2 ic(card.Cx(), card.y0 + 32.0f * u);
    c.dl->AddCircleFilled(ic, 17.0f * u, Rgba(30, 52, 26), 28);
    GlyphHandset(c.dl, ic, 19.0f * u, kLime);
    Text(c, c.fb, L.fsSmall, card.Cx(), card.y0 + 62.0f * u, kText3, "YOUR PHONE NUMBER", 0.5f);
    Text(c, c.fh, 27.0f * u, card.Cx(), card.y0 + 88.0f * u, kText, displayNumber, 0.5f);
    Text(c, c.fb, L.fsSmall, card.Cx(), card.y0 + 110.0f * u, kText2, "SKR Mobile", 0.5f);

    const bool copied = c.v.phone.copiedAt && c.now - c.v.phone.copiedAt < 1800;
    const Rect b{card.Cx() - 52.0f * u, card.y1 + 14.0f * u, card.Cx() + 52.0f * u, card.y1 + 40.0f * u};
    Button(c, b, copied ? "Copied" : "Copy number", Action::CopyNumber, 0, copied ? kLimeDeep : kLime, Rgba(14, 26, 8));

    Text(c, c.fb, L.fsSmall, P.Cx(), b.y1 + 22.0f * u, kText3, "Share it so others can text you.", 0.5f);
    Text(c, c.fb, L.fsSmall, P.Cx(), b.y1 + 36.0f * u, kText3, "Voice calls are not available yet.", 0.5f);
}

// Index list of contacts matching the search text (fixed storage, no allocation).
int FilterContacts(const Ctx& c, uint8_t (&out)[kMaxContacts]) {
    const char* q = c.v.contacts.search.text;
    int n = 0;
    for (int i = 0; i < c.m.contactCount && n < kMaxContacts; ++i)
        if (ContainsNoCase(c.m.contacts[i].name, q) || ContainsNoCase(c.m.contacts[i].number, q))
            out[n++] = static_cast<uint8_t>(i);
    return n;
}

const char* DisplayNumber(const char* number) {
    static char display[16]; protocol::phone::FormatPhoneNumber(number,display); return display;
}

void PageContacts(Ctx& c, const Rect& P) {
    const float u = c.u;
    const Layout& L = c.L;
    PageBackground(c, P);
    const float listTop = L.content.y0 + 78.0f * u, listBottom = P.y1;
    const float rowH = 40.0f * u, secH = 18.0f * u;
    uint8_t idx[kMaxContacts];
    const int count = FilterContacts(c, idx);
    const Rect area{P.x0, listTop, P.x1, L.content.y1};

    ContactsView& view = c.v.contacts;
    if (c.m.sync != SyncState::Ready) {
        ServiceUnavailable(c, area);
    } else if (!count) {
        if (c.m.contactsLoading) EmptyState(c, area, "Loading contacts...", "", true, false);
        else if (view.search.len) EmptyState(c, area, "No matches", view.search.text, false, false);
        else EmptyState(c, area, "No contacts yet", "Tap + to add a number", false, false);
    } else {
        float total = 0;
        for (int k = 0; k < count; ++k) {
            const Contact& ct = c.m.contacts[idx[k]];
            total += rowH + ((k == 0 || ct.name[0] != c.m.contacts[idx[k - 1]].name[0]) ? secH : 0);
        }
        total += L.navBar.H();
        view.scroll = std::clamp(view.scroll, 0.0f, (std::max)(0.0f, total - (listBottom - listTop)));
        float y = listTop - view.scroll;
        for (int k = 0; k < count; ++k) {
            const int i = idx[k];
            const Contact& ct = c.m.contacts[i];
            if (k == 0 || ct.name[0] != c.m.contacts[idx[k - 1]].name[0]) {
                char letter[2] = {ct.name[0], 0};
                if (letter[0] >= 'a' && letter[0] <= 'z') letter[0] = static_cast<char>(letter[0] - 'a' + 'A');
                if (y + secH > listTop - secH && y < listBottom) Text(c, c.fh, L.fsSmall, P.x0 + 16.0f * u, y + secH * 0.55f, kLime, letter);
                y += secH;
            }
            if (y + rowH >= listTop && y < listBottom) {
                const Rect row{P.x0 + 6.0f * u, y, P.x1 - 6.0f * u, y + rowH};
                const uint8_t id = static_cast<uint8_t>(i);
                const bool hov = Hot(c, row, Action::OpenContact, id);
                const float pr = Pressed(c, Action::OpenContact, id);
                if (hov || pr > 0) RoundRect(c.dl, row, Rgba(255, 255, 255, 14 + static_cast<int>(26 * pr)), 10.0f * u);
                Avatar(c, V(row.x0 + 21.0f * u, row.Cy()), 14.0f * u, ct.hue, ct.name);
                char buf[32];
                Text(c, c.fb, L.fsBody, row.x0 + 42.0f * u, row.Cy() - 6.5f * u, kText,
                     Fit(c, c.fb, L.fsBody, ct.name, row.W() - 70.0f * u, buf, sizeof(buf)));
                Text(c, c.fb, L.fsSmall, row.x0 + 42.0f * u, row.Cy() + 8.0f * u, kText2, DisplayNumber(ct.number));
                GlyphChevron(c.dl, V(row.x1 - 12.0f * u, row.Cy()), 11.0f * u, hov ? kLime : kText3, false, 1.5f * u);
                if (k + 1 < count && c.m.contacts[idx[k + 1]].name[0] == ct.name[0])
                    c.dl->AddLine(V(row.x0 + 42.0f * u, row.y1), V(row.x1 - 8.0f * u, row.y1), kHair, 1.0f);
            }
            y += rowH;
        }
        Scrollbar(c, P, listTop, listBottom - L.navBar.H(), total, view.scroll);
    }

    char countText[16];
    std::snprintf(countText, sizeof(countText), "%d", c.m.contactCount);
    const float hb = Header(c, P, "Contacts", true, 74.0f * u);
    if (c.m.sync == SyncState::Ready) {
        const ImVec2 plus(P.x1 - 22.0f * u, L.content.y0 + 19.0f * u);
        const bool hov = HeaderIcon(c, plus, Action::AddContact, 0);
        GlyphPlus(c.dl, plus, 16.0f * u, hov ? kLime : kText, 1.8f * u);
        if (c.m.contactCount) Text(c, c.fb, L.fsSmall, plus.x - 20.0f * u, plus.y, kText3, countText, 1.0f);
    }
    const Rect search{P.x0 + 12.0f * u, hb - 32.0f * u, P.x1 - 12.0f * u, hb - 8.0f * u};
    InputField(c, search, view.search, "Search", Field::ContactSearch, L.fsSmall, 26.0f);
    GlyphSearch(c.dl, V(search.x0 + 13.0f * u, search.Cy()), 12.0f * u, kText3, 1.4f * u);
}

const Contact* DetailContact(const Ctx& c) {
    for (int i = 0; i < c.m.contactCount; ++i)
        if (std::strcmp(c.m.contacts[i].number, c.v.contacts.detailNumber) == 0) return &c.m.contacts[i];
    return nullptr;
}

void PageContactDetail(Ctx& c, const Rect& P) {
    const float u = c.u;
    const Layout& L = c.L;
    PageBackground(c, P);
    const float hb = Header(c, P, "Contact", true, 38.0f * u);
    const Contact* ct = DetailContact(c);
    if (!ct) {
        EmptyState(c, {P.x0, hb, P.x1, L.content.y1}, "Contact removed", "", false, false);
        return;
    }
    const ImVec2 av(P.Cx(), hb + 46.0f * u);
    Avatar(c, av, 28.0f * u, ct->hue, ct->name);
    char buf[32];
    Text(c, c.fh, 19.0f * u, P.Cx(), av.y + 44.0f * u, kText, Fit(c, c.fh, 19.0f * u, ct->name, P.W() - 30.0f * u, buf, sizeof(buf)), 0.5f);
    Text(c, c.fb, L.fsBody, P.Cx(), av.y + 63.0f * u, kText2, DisplayNumber(ct->number), 0.5f);

    const float bw = P.W() - 40.0f * u;
    const Rect msg{P.Cx() - bw * 0.5f, av.y + 88.0f * u, P.Cx() + bw * 0.5f, av.y + 116.0f * u};
    Button(c, msg, "Send message", Action::MessageContact, 0, kLime, Rgba(14, 26, 8), !c.m.contactOpPending);
    const Rect rm{msg.x0, msg.y1 + 10.0f * u, msg.x1, msg.y1 + 38.0f * u};
    if (c.m.contactOpPending) {
        Spinner(c, V(rm.Cx(), rm.Cy()), 8.0f * u, kCoral);
    } else {
        const bool hov = Hot(c, rm, Action::RemoveContact, 0);
        RoundRect(c.dl, rm, hov ? WithAlpha(kCoral, 0.18f) : Rgba(255, 255, 255, 8), rm.H() * 0.5f);
        c.dl->AddRect(V(rm.x0, rm.y0), V(rm.x1, rm.y1), WithAlpha(kCoral, hov ? 0.9f : 0.5f), rm.H() * 0.5f, 0, 1.0f);
        Text(c, c.fh, L.fsBody * 0.92f, rm.Cx(), rm.Cy(), kCoral, "Remove contact", 0.5f);
    }
}

void PageContactAdd(Ctx& c, const Rect& P) {
    const float u = c.u;
    const Layout& L = c.L;
    PageBackground(c, P);
    const float hb = Header(c, P, "New Contact", true, 38.0f * u);
    if (ServiceUnavailable(c, {P.x0, hb, P.x1, L.content.y1})) return;
    const ContactsView& v = c.v.contacts;
    float y = hb + 22.0f * u;
    const float x0 = P.x0 + 14.0f * u, x1 = P.x1 - 14.0f * u;
    Text(c, c.fh, L.fsSmall * 0.92f, x0 + 2.0f * u, y, kText3, "NAME");
    InputField(c, {x0, y + 9.0f * u, x1, y + 37.0f * u}, v.name, "Contact name", Field::ContactName, L.fsBody);
    y += 56.0f * u;
    Text(c, c.fh, L.fsSmall * 0.92f, x0 + 2.0f * u, y, kText3, "PHONE NUMBER");
    InputField(c, {x0, y + 9.0f * u, x1, y + 37.0f * u}, v.number, "7 digits", Field::ContactNumber, L.fsBody);
    char hint[24];
    std::snprintf(hint, sizeof(hint), "%u / 7", static_cast<unsigned>(v.number.len));
    Text(c, c.fb, L.fsSmall * 0.92f, x1, y + 48.0f * u, v.number.len == 7 ? kLime : kText3, hint, 1.0f);
    y += 70.0f * u;
    const bool ready = v.number.len == 7 && !c.m.contactOpPending;
    const Rect save{x0, y, x1, y + 30.0f * u};
    if (c.m.contactOpPending) {
        RoundRect(c.dl, save, kSurfaceHi, save.H() * 0.5f);
        Spinner(c, V(save.Cx(), save.Cy()), 7.0f * u, kLime);
    } else {
        Button(c, save, "Save contact", Action::SaveContact, 0, kLime, Rgba(14, 26, 8), ready);
    }
    Text(c, c.fb, L.fsSmall, P.Cx(), save.y1 + 18.0f * u, kText3, "Enter: next field / save   Esc: done", 0.5f);
}

void PageThreads(Ctx& c, const Rect& P) {
    const float u = c.u;
    const Layout& L = c.L;
    PageBackground(c, P);
    const float top = L.content.y0 + 44.0f * u, rowH = 50.0f * u;
    const Rect area{P.x0, top, P.x1, L.content.y1};
    MessagesView& view = c.v.messages;
    if (c.m.sync != SyncState::Ready) {
        ServiceUnavailable(c, area);
    } else if (!c.m.threadCount) {
        if (c.m.threadsLoading) EmptyState(c, area, "Loading messages...", "", true, false);
        else EmptyState(c, area, "No conversations", "Tap the pencil to write one", false, false);
    } else {
        const float total = c.m.threadCount * rowH + L.navBar.H();
        view.listScroll = std::clamp(view.listScroll, 0.0f, (std::max)(0.0f, total - (P.y1 - top)));
        float y = top - view.listScroll;
        for (int i = 0; i < c.m.threadCount; ++i, y += rowH) {
            if (y + rowH < top || y > P.y1) continue;
            const ThreadSummary& t = c.m.threads[i];
            const Rect row{P.x0 + 6.0f * u, y, P.x1 - 6.0f * u, y + rowH};
            const uint8_t idx = static_cast<uint8_t>(i);
            const bool hov = Hot(c, row, Action::OpenThread, idx);
            const float pr = Pressed(c, Action::OpenThread, idx);
            if (hov || pr > 0) SurfaceCard(c, row, 10.0f * u, hov ? 1.0f : pr, t.unread ? kLime : 0);
            Avatar(c, V(row.x0 + 22.0f * u, row.Cy()), 16.0f * u, t.hue, t.peer);
            char buf[64];
            const float tx = row.x0 + 46.0f * u, tw = row.x1 - tx - 40.0f * u;
            Text(c, t.unread ? c.fh : c.fb, L.fsBody, tx, row.Cy() - 7.0f * u, kText, Fit(c, c.fb, L.fsBody, t.peer, tw, buf, sizeof(buf)));
            Text(c, c.fb, L.fsSmall, tx, row.Cy() + 8.0f * u, t.unread ? kText2 : kText3, Fit(c, c.fb, L.fsSmall, t.preview, tw, buf, sizeof(buf)));
            Text(c, c.fb, L.fsSmall * 0.92f, row.x1 - 8.0f * u, row.Cy() - 7.0f * u, t.unread ? kLime : kText3, t.time, 1.0f);
            if (t.unread) c.dl->AddCircleFilled(V(row.x1 - 12.0f * u, row.Cy() + 8.0f * u), 4.0f * u, kLime, 12);
            if (i + 1 < c.m.threadCount) c.dl->AddLine(V(tx, row.y1), V(row.x1 - 8.0f * u, row.y1), kHair, 1.0f);
        }
        Scrollbar(c, P, top, P.y1 - L.navBar.H(), total, view.listScroll);
    }
    Header(c, P, "Messages", true, 40.0f * u);
    if (c.m.sync == SyncState::Ready) {
        const ImVec2 pc(P.x1 - 22.0f * u, L.content.y0 + 19.0f * u);
        const bool hov = HeaderIcon(c, pc, Action::Compose, 0);
        GlyphPencil(c.dl, pc, 15.0f * u, hov ? kLime : kText2, 1.4f * u);
        if (c.m.threadsLoading && c.m.threadCount) Spinner(c, V(pc.x - 26.0f * u, pc.y), 5.0f * u, kText3);
    }
}

void PageConversation(Ctx& c, const Rect& P) {
    const float u = c.u;
    const Layout& L = c.L;
    const Conversation& cv = c.m.convo;
    MessagesView& view = c.v.messages;
    PageBackground(c, P);

    const float composerH = 40.0f * u;
    const float top = L.content.y0 + 46.0f * u, bottom = L.content.y1 - composerH;
    const float maxW = P.W() * 0.72f, padX = 9.0f * u, padY = 6.0f * u, gap = 5.0f * u, fs = L.fsBody;

    // Measure the whole history once so the scroll range is exact.
    float content = 8.0f * u;
    int lastOut = -1;
    for (int i = cv.count - 1; i >= 0; --i)
        if (cv.items[i].delivery != Delivery::Received) { lastOut = i; break; }
    for (int i = 0; i < cv.count; ++i) {
        const ImVec2 ts = Pick(c, c.fb, fs)->CalcTextSizeA(fs, FLT_MAX, maxW - padX * 2, cv.items[i].text);
        content += ts.y + padY * 2 + gap;
        if (i == lastOut) content += 13.0f * u;
        if (i == 0 || MinutesBetween(cv.items[i - 1].time, cv.items[i].time) >= 5) content += 16.0f * u;
    }
    if (cv.hasMore || cv.loading) content += 30.0f * u;
    view.scroll = std::clamp(view.scroll, 0.0f, (std::max)(0.0f, content - (bottom - top)));

    float y = bottom - 6.0f * u + view.scroll;
    for (int i = cv.count - 1; i >= 0 && y > top - 60.0f * u; --i) {
        const Message& msg = cv.items[i];
        const bool out = msg.delivery != Delivery::Received;
        if (i == lastOut) {
            // "Sent" = stored by the server; delivery to the other phone is not claimed.
            const char* st = msg.delivery == Delivery::Sending ? "Sending..." : msg.delivery == Delivery::Delivered ? "Sent" : "Not sent";
            const ImU32 sc = msg.delivery == Delivery::Failed ? kCoral : kText3;
            y -= 13.0f * u;
            Text(c, c.fb, L.fsSmall * 0.92f, P.x1 - 12.0f * u, y + 6.0f * u, sc, st, 1.0f);
        }
        const ImVec2 ts = Pick(c, c.fb, fs)->CalcTextSizeA(fs, FLT_MAX, maxW - padX * 2, msg.text);
        const float bw = (std::min)(maxW, ts.x + padX * 2), bh = ts.y + padY * 2;
        y -= bh;
        const float bx = out ? P.x1 - 10.0f * u - bw : P.x0 + 10.0f * u;
        const Rect b{bx, y, bx + bw, y + bh};
        const float rad = 12.0f * u;
        if (out) {
            const ImU32 base = msg.delivery == Delivery::Failed ? Mix(kCoral, kSurfaceHi, 0.35f) : kLime;
            const int v0 = c.dl->VtxBuffer.Size;
            RoundRect(c.dl, b, base, rad);
            Gradient(c.dl, v0, b, Mix(base, Rgba(255, 255, 255), 0.08f), Mix(base, kLimeDeep, 0.35f));
            if (msg.delivery == Delivery::Sending) RoundRect(c.dl, b, Rgba(9, 13, 20, 70), rad);
        } else {
            const int v0 = c.dl->VtxBuffer.Size;
            RoundRect(c.dl, b, kSurfaceHi, rad);
            Gradient(c.dl, v0, b, Rgba(35, 47, 65), kSurfaceHi);
            c.dl->AddRect(V(b.x0, b.y0), V(b.x1, b.y1), Rgba(255, 255, 255, 18), rad, 0, 1.0f);
        }
        c.dl->AddText(Pick(c, c.fb, fs), fs, V(std::round(b.x0 + padX), std::round(b.y0 + padY)), out ? Rgba(14, 26, 8) : kText,
                      msg.text, nullptr, maxW - padX * 2);
        y -= gap;
        if (i == 0 || MinutesBetween(cv.items[i - 1].time, msg.time) >= 5) {
            y -= 12.0f * u;
            Text(c, c.fb, L.fsSmall * 0.9f, P.Cx(), y + 6.0f * u, kText3, msg.time, 0.5f);
            y -= 4.0f * u;
        }
    }
    if (cv.loading) {
        Spinner(c, V(P.Cx(), y - 14.0f * u), 6.0f * u, kText3);
    } else if (cv.hasMore) {
        const Rect more{P.Cx() - 46.0f * u, y - 26.0f * u, P.Cx() + 46.0f * u, y - 6.0f * u};
        if (more.y0 >= top) {
            const bool hov = Hot(c, more, Action::LoadOlder, 0);
            RoundRect(c.dl, more, Rgba(255, 255, 255, hov ? 30 : 16), more.H() * 0.5f);
            Text(c, c.fb, L.fsSmall, more.Cx(), more.Cy(), hov ? kLime : kText2, "Earlier messages", 0.5f);
        }
    }
    if (!cv.count && !cv.loading) {
        if (c.m.sync != SyncState::Ready) ServiceUnavailable(c, {P.x0, top, P.x1, bottom});
        else EmptyState(c, {P.x0, top, P.x1, bottom}, "No messages yet", "Say hello", false, false);
    }
    Scrollbar(c, P, top, bottom, content, content - (bottom - top) - view.scroll);

    // Composer band.
    const Rect band{P.x0, L.content.y1 - composerH, P.x1, P.y1};
    RoundRect(c.dl, band, kPage, c.L.screenRadius, ImDrawFlags_RoundCornersBottom);
    c.dl->AddLine(V(band.x0, band.y0), V(band.x1, band.y0), kHair, 1.0f);
    const Rect field{P.x0 + 10.0f * u, band.y0 + 7.0f * u, P.x1 - 44.0f * u, band.y0 + 33.0f * u};
    InputField(c, field, view.draft, "Message", Field::SmsDraft, L.fsSmall * 1.08f);
    if (view.draft.len > 90) {
        char left[8];
        std::snprintf(left, sizeof(left), "%d", static_cast<int>(kFieldSmsMax) - view.draft.len);
        Text(c, c.fb, L.fsSmall * 0.85f, field.x1 - 8.0f * u, field.y0 - 5.0f * u, view.draft.len >= 115 ? kAmber : kText3, left, 1.0f);
    }
    const bool canSend = TextHasContent(view.draft) && c.m.sync == SyncState::Ready;
    const ImVec2 sb(P.x1 - 24.0f * u, field.Cy());
    const Rect sendHit{sb.x - 13.0f * u, sb.y - 13.0f * u, sb.x + 13.0f * u, sb.y + 13.0f * u};
    const bool sh = canSend && Hot(c, sendHit, Action::SendSms, 0);
    c.dl->AddCircleFilled(sb, 13.0f * u, !canSend ? kSurfaceHi : sh ? Mix(kLime, Rgba(255, 255, 255), 0.2f) : kLime, 24);
    GlyphArrowUp(c.dl, sb, 15.0f * u, canSend ? Rgba(14, 26, 8) : kText3, 1.8f * u);

    // Header with peer.
    Header(c, P, nullptr, true, 40.0f * u);
    const float cy = L.content.y0 + 19.0f * u;
    Avatar(c, V(P.x0 + 50.0f * u, cy), 12.0f * u, cv.hue, cv.peer);
    char buf[32];
    // Unsaved sender: the name IS the number, so show it once, centred.
    const bool named = std::strcmp(cv.peer, cv.number) != 0;
    Text(c, c.fh, L.fsBody, P.x0 + 68.0f * u, named ? cy - 6.0f * u : cy, kText,
         Fit(c, c.fh, L.fsBody, cv.peer, P.x1 - P.x0 - 80.0f * u, buf, sizeof(buf)));
    if (named) Text(c, c.fb, L.fsSmall * 0.92f, P.x0 + 68.0f * u, cy + 8.0f * u, kText2, DisplayNumber(cv.number));
}

// New message: recipient by number or from contacts.
void PageCompose(Ctx& c, const Rect& P) {
    const float u = c.u;
    const Layout& L = c.L;
    PageBackground(c, P);
    const MessagesView& v = c.v.messages;
    const float listTop = L.content.y0 + 96.0f * u;
    const float rowH = 36.0f * u;
    float y = listTop;
    if (c.m.sync == SyncState::Ready && c.m.contactCount) {
        Text(c, c.fh, L.fsSmall * 0.9f, P.x0 + 16.0f * u, y + 2.0f * u, kText3, "CONTACTS");
        y += 12.0f * u;
        for (int i = 0; i < c.m.contactCount && y + rowH < L.content.y1; ++i) {
            const Contact& ct = c.m.contacts[i];
            if (v.to.len && std::strncmp(ct.number, v.to.text, v.to.len) != 0) continue;
            const Rect row{P.x0 + 6.0f * u, y, P.x1 - 6.0f * u, y + rowH};
            const uint8_t idx = static_cast<uint8_t>(i + 1);
            const bool hov = Hot(c, row, Action::ComposeNext, idx);
            if (hov) RoundRect(c.dl, row, Rgba(255, 255, 255, 14), 10.0f * u);
            Avatar(c, V(row.x0 + 19.0f * u, row.Cy()), 12.0f * u, ct.hue, ct.name);
            char buf[32];
            Text(c, c.fb, L.fsBody, row.x0 + 38.0f * u, row.Cy() - 6.0f * u, kText, Fit(c, c.fb, L.fsBody, ct.name, row.W() - 50.0f * u, buf, sizeof(buf)));
            Text(c, c.fb, L.fsSmall, row.x0 + 38.0f * u, row.Cy() + 7.5f * u, kText2, DisplayNumber(ct.number));
            y += rowH;
        }
    }
    const float hb = Header(c, P, "New Message", true, 90.0f * u);
    if (c.m.sync != SyncState::Ready) {
        ServiceUnavailable(c, {P.x0, hb, P.x1, L.content.y1});
        return;
    }
    const Rect to{P.x0 + 12.0f * u, hb - 40.0f * u, P.x1 - 50.0f * u, hb - 12.0f * u};
    Text(c, c.fh, L.fsSmall * 0.9f, to.x0 + 2.0f * u, to.y0 - 8.0f * u, kText3, "TO");
    InputField(c, to, v.to, "Phone number", Field::NewMessageTo, L.fsBody);
    const ImVec2 nb(P.x1 - 28.0f * u, to.Cy());
    const bool ok = v.to.len == 7;
    const bool hov = ok && Hot(c, {nb.x - 14.0f * u, nb.y - 14.0f * u, nb.x + 14.0f * u, nb.y + 14.0f * u}, Action::ComposeNext, 0);
    c.dl->AddCircleFilled(nb, 13.0f * u, !ok ? kSurfaceHi : hov ? Mix(kLime, Rgba(255, 255, 255), 0.2f) : kLime, 24);
    GlyphChevron(c.dl, nb, 13.0f * u, ok ? Rgba(14, 26, 8) : kText3, false, 1.8f * u);
}

// Confirmation sheet over the contact details page.
void DrawRemoveConfirm(Ctx& c) {
    const float u = c.u;
    const Layout& L = c.L;
    const Contact* ct = DetailContact(c);
    RoundRect(c.dl, L.screen, Rgba(0, 0, 0, 150), L.screenRadius);
    Hot(c, L.screen, Action::CancelRemove, 1);      // tap outside the sheet = cancel; blocks the page below
    const Rect sheet{L.screen.x0 + 12.0f * u, L.content.Cy() - 62.0f * u, L.screen.x1 - 12.0f * u, L.content.Cy() + 62.0f * u};
    SoftShadow(c.dl, sheet.Offset(0, 3.0f * u), 16.0f * u, 10.0f * u, Rgba(0, 0, 0, 180));
    RoundRect(c.dl, sheet, Rgba(28, 36, 50), 16.0f * u);
    Text(c, c.fh, L.fsBody * 1.05f, sheet.Cx(), sheet.y0 + 24.0f * u, kText, "Remove contact?", 0.5f);
    char buf[40];
    if (ct) Text(c, c.fb, L.fsSmall, sheet.Cx(), sheet.y0 + 44.0f * u, kText2, Fit(c, c.fb, L.fsSmall, ct->name, sheet.W() - 24.0f * u, buf, sizeof(buf)), 0.5f);
    Text(c, c.fb, L.fsSmall, sheet.Cx(), sheet.y0 + 60.0f * u, kText3, "Messages are kept.", 0.5f);
    const float bw = (sheet.W() - 36.0f * u) * 0.5f;
    const Rect no{sheet.x0 + 12.0f * u, sheet.y1 - 40.0f * u, sheet.x0 + 12.0f * u + bw, sheet.y1 - 14.0f * u};
    const Rect yes{no.x1 + 12.0f * u, no.y0, sheet.x1 - 12.0f * u, no.y1};
    Button(c, no, "Cancel", Action::CancelRemove, 0, kSurfaceHi, kText);
    Button(c, yes, "Remove", Action::ConfirmRemove, 0, kCoral, Rgba(255, 255, 255));
}

// Short status line above the gesture bar (request errors, confirmations).
void DrawNotice(Ctx& c) {
    if (!c.m.notice[0] || !c.m.noticeAt || c.now < c.m.noticeAt) return;
    const uint64_t age = c.now - c.m.noticeAt;
    if (age > 2800) return;
    const float a = age < 160 ? age / 160.0f : age > 2400 ? (2800 - age) / 400.0f : 1.0f;
    const float u = c.u;
    const Layout& L = c.L;
    char buf[64];
    const char* s = Fit(c, c.fb, L.fsSmall, c.m.notice, L.screen.W() - 44.0f * u, buf, sizeof(buf));
    const float tw = Measure(c, c.fb, L.fsSmall, s).x;
    // Under the page header: never over the composer or list actions.
    const Rect r{L.screen.Cx() - tw * 0.5f - 12.0f * u, L.content.y0 + 48.0f * u, L.screen.Cx() + tw * 0.5f + 12.0f * u, L.content.y0 + 70.0f * u};
    const int v0 = c.dl->VtxBuffer.Size;
    RoundRect(c.dl, r, c.m.noticeError ? Rgba(70, 24, 32, 240) : Rgba(28, 44, 30, 240), r.H() * 0.5f);
    c.dl->AddRect(V(r.x0, r.y0), V(r.x1, r.y1), c.m.noticeError ? WithAlpha(kCoral, 0.6f) : WithAlpha(kLime, 0.5f), r.H() * 0.5f, 0, 1.0f);
    Text(c, c.fb, L.fsSmall, r.Cx(), r.Cy(), kText, s, 0.5f);
    if (a < 0.999f) Transform(c.dl, v0, V(0, 0), 1.0f, V(0, 0), (std::max)(0.0f, a));
}

void PageGps(Ctx& c, const Rect& P) {
    const float u = c.u;
    const Layout& L = c.L;
    const GpsInfo& g = c.m.gps;
    PageBackground(c, P);
    float y = L.content.y0 + 48.0f * u;

    // Current destination card.
    const Rect card{P.x0 + 10.0f * u, y, P.x1 - 10.0f * u, y + 104.0f * u};
    SurfaceCard(c, card, 14.0f * u, 0.0f, g.status == NavStatus::Idle ? kText3 : kLime);
    const float gx = card.x0 + 20.0f * u;
    c.dl->AddCircleFilled(V(gx, card.y0 + 22.0f * u), 5.0f * u, kCyan, 16);
    c.dl->AddCircle(V(gx, card.y0 + 22.0f * u), 8.0f * u, WithAlpha(kCyan, 0.35f), 16, 1.0f);
    for (float dy = 34.0f; dy < 62.0f; dy += 6.0f)
        c.dl->AddCircleFilled(V(gx, card.y0 + dy * u), 1.3f * u, WithAlpha(kText2, 0.8f), 8);
    GlyphPin(c.dl, V(gx, card.y0 + 72.0f * u), 18.0f * u, g.status == NavStatus::Idle ? kText3 : kLime, Rgba(20, 28, 40));
    const float tx = card.x0 + 38.0f * u, tw = card.x1 - tx - 10.0f * u;
    char buf[48], dist[24];
    if (g.status == NavStatus::Idle) {
        Text(c, c.fh, L.fsBody, tx, card.y0 + 26.0f * u, kText, "No active route");
        Text(c, c.fb, L.fsSmall, tx, card.y0 + 44.0f * u, kText2, "Set a waypoint on the ESC map");
    } else {
        FormatDistance(g.remainingM, dist, sizeof(dist));
        Text(c, c.fh, 26.0f * u, tx, card.y0 + 26.0f * u, kText, g.status==NavStatus::Navigating ? dist : g.status==NavStatus::Calculating ? "Calculating" : g.status==NavStatus::Arrived ? "Arrived" : "No route");
        Text(c, c.fh, L.fsBody, tx, card.y0 + 52.0f * u, kText, Fit(c, c.fh, L.fsBody, g.destName, tw, buf, sizeof(buf)));
        Text(c, c.fb, L.fsSmall, tx, card.y0 + 68.0f * u, kText2, Fit(c, c.fb, L.fsSmall, g.destArea, tw, buf, sizeof(buf)));
        const Rect clr{card.x1 - 74.0f * u, card.y1 - 26.0f * u, card.x1 - 10.0f * u, card.y1 - 8.0f * u};
        const bool hov = Hot(c, clr, Action::ClearRoute, 0);
        RoundRect(c.dl, clr, hov ? WithAlpha(kCoral, 0.22f) : Rgba(255, 255, 255, 10), clr.H() * 0.5f);
        c.dl->AddRect(V(clr.x0, clr.y0), V(clr.x1, clr.y1), WithAlpha(kCoral, hov ? 0.9f : 0.55f), clr.H() * 0.5f, 0, 1.0f);
        Text(c, c.fh, L.fsSmall * 0.9f, clr.Cx(), clr.Cy(), kCoral, "Clear route", 0.5f);
    }
    y = card.y1 + 18.0f * u;
    Text(c, c.fh, L.fsSmall * 0.9f, P.x0 + 16.0f * u, y, kText3, "DESTINATIONS");
    y += 10.0f * u;

    if (!g.count) Text(c,c.fb,L.fsSmall,P.x0+16.0f*u,y+12.0f*u,kText2,"Use the ESC map to place a waypoint");
    const float rowH = 34.0f * u;
    for (int i = 0; i < g.count && y < P.y1; ++i, y += rowH) {
        const Destination& d = g.list[i];
        const Rect row{P.x0 + 6.0f * u, y, P.x1 - 6.0f * u, y + rowH};
        const uint8_t idx = static_cast<uint8_t>(i);
        const bool active = g.status != NavStatus::Idle && std::strcmp(d.name, g.destName) == 0;
        const bool hov = Hot(c, row, Action::SelectDestination, idx) || c.v.gps.selected == i;
        if (hov || active) RoundRect(c.dl, row, active ? WithAlpha(kLime, 0.10f) : Rgba(255, 255, 255, 14), 9.0f * u);
        GlyphPin(c.dl, V(row.x0 + 15.0f * u, row.Cy()), 15.0f * u, active ? kLime : kText2, active ? Rgba(22, 34, 22) : kPage);
        Text(c, c.fb, L.fsBody, row.x0 + 30.0f * u, row.Cy() - 6.0f * u, kText, Fit(c, c.fb, L.fsBody, d.name, row.W() - 90.0f * u, buf, sizeof(buf)));
        Text(c, c.fb, L.fsSmall * 0.92f, row.x0 + 30.0f * u, row.Cy() + 7.5f * u, kText3, Fit(c, c.fb, L.fsSmall, d.area, row.W() - 90.0f * u, buf, sizeof(buf)));
        FormatDistance(d.distanceM, dist, sizeof(dist));
        Text(c, c.fh, L.fsSmall, row.x1 - 10.0f * u, row.Cy(), active ? kLime : kText2, dist, 1.0f);
    }

    if (!c.m.previewData) Button(c,{P.x0+14*u,y+36*u,P.x1-14*u,y+66*u},"Open map / set waypoint",Action::OpenGpsMap,0,kLime,Rgba(14,26,8));
    static const char* kStatus[] = {"IDLE", "CALCULATING", "NAVIGATING", "ARRIVED", "UNAVAILABLE"};
    Header(c, P, "GPS", true, 40.0f * u);
    const char* st = kStatus[static_cast<int>(g.status)];
    const ImU32 sc = g.status == NavStatus::Navigating || g.status == NavStatus::Arrived ? kLime
                     : g.status == NavStatus::Calculating ? kAmber : kText3;
    const float sw = Measure(c, c.fh, 8.0f * u, st).x + 14.0f * u;
    const Rect chip{P.x1 - 12.0f * u - sw, L.content.y0 + 11.0f * u, P.x1 - 12.0f * u, L.content.y0 + 27.0f * u};
    RoundRect(c.dl, chip, WithAlpha(sc, 0.14f), chip.H() * 0.5f);
    Text(c, c.fh, 8.0f * u, chip.Cx(), chip.Cy(), sc, st, 0.5f);
}

void Toggle(Ctx& c, ImVec2 ctr, bool on) {
    const float u = c.u, w = 30.0f * u, h = 17.0f * u;
    const Rect t{ctr.x - w * 0.5f, ctr.y - h * 0.5f, ctr.x + w * 0.5f, ctr.y + h * 0.5f};
    RoundRect(c.dl, t, on ? kLime : Rgba(60, 70, 86), h * 0.5f);
    const ImVec2 k(on ? t.x1 - h * 0.5f : t.x0 + h * 0.5f, ctr.y);
    c.dl->AddCircleFilled(k, h * 0.5f - 2.0f * u, Rgba(250, 252, 255), 20);
}

void PageSettings(Ctx& c, const Rect& P) {
    const float u = c.u;
    const Layout& L = c.L;
    const PhoneSettings& s = c.m.settings;
    char displayNumber[16]; protocol::phone::FormatPhoneNumber(c.m.ownNumber,displayNumber);
    PageBackground(c, P);
    float y = L.content.y0 + 62.0f * u;   // section label sits below the 40u header
    const float rowH = 34.0f * u;
    auto group = [&](int rows) {
        const Rect g{P.x0 + 10.0f * u, y, P.x1 - 10.0f * u, y + rows * rowH};
        RoundRect(c.dl, g, kSurface, 12.0f * u);
        return g;
    };
    auto label = [&](const Rect& g, int r, const char* text) {
        const float cy = g.y0 + rowH * (r + 0.5f);
        Text(c, c.fb, L.fsBody, g.x0 + 12.0f * u, cy, kText, text);
        if (r > 0) c.dl->AddLine(V(g.x0 + 12.0f * u, g.y0 + rowH * r), V(g.x1, g.y0 + rowH * r), kHair, 1.0f);
        return cy;
    };

    Text(c, c.fh, L.fsSmall * 0.9f, P.x0 + 16.0f * u, y - 8.0f * u, kText3, "DISPLAY");
    Rect g = group(3);
    float cy = label(g, 0, "Phone size");
    if (c.m.profileLoaded && !c.m.profilePending) Hot(c,{g.x0,g.y0,g.x1,g.y0+rowH},Action::ToggleSetting,3);
    char pct[16];
    std::snprintf(pct, sizeof(pct), "%d%%", static_cast<int>(std::lround(s.uiScale * 100.0f)));
    Text(c, c.fh, L.fsSmall, g.x1 - 12.0f * u, cy, kLime, pct, 1.0f);
    const float bx0 = g.x1 - 98.0f * u, bx1 = g.x1 - 46.0f * u;
    RoundRect(c.dl, {bx0, cy - 1.5f * u, bx1, cy + 1.5f * u}, Rgba(60, 70, 86), 1.5f * u);
    const float kx = bx0 + (bx1 - bx0) * std::clamp((s.uiScale - kMinUiScale) / (kMaxUiScale - kMinUiScale), 0.0f, 1.0f);
    RoundRect(c.dl, {bx0, cy - 1.5f * u, kx, cy + 1.5f * u}, kLime, 1.5f * u);
    c.dl->AddCircleFilled(V(kx, cy), 5.5f * u, Rgba(250, 252, 255), 16);
    cy = label(g, 1, "Open phone");
    const float kw = Measure(c, c.fh, L.fsSmall, s.openKey).x + 12.0f * u;
    const Rect key{g.x1 - 12.0f * u - kw, cy - 9.0f * u, g.x1 - 12.0f * u, cy + 9.0f * u};
    RoundRect(c.dl, key, Rgba(255, 255, 255, 20), 5.0f * u);
    c.dl->AddRect(V(key.x0, key.y0), V(key.x1, key.y1), Rgba(255, 255, 255, 40), 5.0f * u, 0, 1.0f);
    Text(c, c.fh, L.fsSmall, key.Cx(), key.Cy(), kText, s.openKey, 0.5f);

    cy=label(g,2,"Wallpaper");
    Text(c,c.fb,L.fsSmall,g.x1-12.0f*u,cy,kText2,c.m.profile.wallpaper?"Skyline":"iFruit",1.0f);
    if (c.m.profileLoaded && !c.m.profilePending) Hot(c,{g.x0,g.y0+2*rowH,g.x1,g.y1},Action::ToggleSetting,4);
    y = g.y1 + 24.0f * u;
    Text(c, c.fh, L.fsSmall * 0.9f, P.x0 + 16.0f * u, y - 8.0f * u, kText3, "ALERTS");
    g = group(1);
    const char* names[3] = {"Banners", "Sounds", "Vibration"};
    const bool vals[3] = {s.banners, s.sounds, s.vibrate};
    for (int r = 0; r < 1; ++r) {
        cy = label(g, r, names[r]);
        const Rect hit{g.x0, g.y0 + rowH * r, g.x1, g.y0 + rowH * (r + 1)};
        if (c.m.profileLoaded && !c.m.profilePending) Hot(c, hit, Action::ToggleSetting, 2);
        Toggle(c, V(g.x1 - 27.0f * u, cy), vals[r]);
    }
    y = g.y1 + 24.0f * u;
    Text(c, c.fh, L.fsSmall * 0.9f, P.x0 + 16.0f * u, y - 8.0f * u, kText3, "ABOUT");
    g = group(1);
    cy = label(g, 0, "My number");
    Text(c, c.fb, L.fsSmall, g.x1 - 12.0f * u, cy, kText2, displayNumber, 1.0f);

    if (!c.m.profileLoaded) Text(c,c.fb,L.fsSmall,P.Cx(),L.content.y1-12*u,kText3,"Preferences need an M5 phone service",0.5f);
    Header(c, P, "Settings", true, 40.0f * u);
}

void DrawPage(Ctx& c, Screen s, const Rect& P) {
    switch (s.app) {
        case AppId::Home: PageHome(c, P); break;
        case AppId::Phone: PagePhone(c, P); break;
        case AppId::Messages:
            if (s.page == 0) PageThreads(c, P);
            else if (c.v.messages.sub == MessagesSub::Compose) PageCompose(c, P);
            else PageConversation(c, P);
            break;
        case AppId::Contacts:
            if (s.page == 0) PageContacts(c, P);
            else if (c.v.contacts.sub == ContactsSub::Add) PageContactAdd(c, P);
            else PageContactDetail(c, P);
            break;
        case AppId::Gps: PageGps(c, P); break;
        case AppId::Settings: PageSettings(c, P); break;
        default: break;
    }
}

// First activation uses the existing shell animation and input owner. Every
// stage is acknowledged and persisted by the server before this view advances.
void DrawSetup(Ctx& c) {
    const Rect P=c.L.screen; const float u=c.u;
    Wallpaper(c,P);
    RoundRect(c.dl,P,Rgba(5,10,18,174),c.L.screenRadius);
    const auto& m=c.m;
    const bool loaded=m.profileLoaded && m.sync==SyncState::Ready;
    const unsigned stage=m.profile.stage;
    const char* titles[]={"Hello", "Language", "Your number", "Make it yours"};
    const char* details[]={"Welcome to your new phone", "English", "Assigned securely by the server", "Choose your wallpaper"};
    const float helloAlpha=loaded && stage==0?0.88f+0.12f*std::sin(static_cast<float>(c.now%4000)*0.0015708f):1.0f;
    const Rect mark{P.Cx()-22*u,P.y0+58*u,P.Cx()+22*u,P.y0+102*u};
    SurfaceCard(c,mark,14*u,0.0f,kLime);
    Text(c,c.fh,14.0f*u,mark.Cx(),mark.Cy(),kLime,"SKR",0.5f);
    Text(c,c.fh,29.0f*u,P.Cx(),P.y0+P.H()*0.34f,WithAlpha(kText,helloAlpha),loaded?titles[stage<4?stage:0]:"Phone service",0.5f);
    Text(c,c.fb,c.L.fsSmall,P.Cx(),P.y0+P.H()*0.42f,kText2,loaded?details[stage<4?stage:0]:m.profilePending?"Connecting securely...":"Service unavailable. Close or retry.",0.5f);
    if (loaded && stage==2) {
        char number[16]; protocol::phone::FormatPhoneNumber(m.ownNumber,number);
        const Rect nr{P.x0+28*u,P.y0+P.H()*0.47f,P.x1-28*u,P.y0+P.H()*0.57f};
        SurfaceCard(c,nr,12*u,0.0f,kLime);
        Text(c,c.fh,24.0f*u,P.Cx(),nr.Cy(),kLime,number,0.5f);
    }
    auto button=[&](float fraction,const char* text,Action action) {
        Rect r{P.x0+22*u,P.y0+P.H()*fraction,P.x1-22*u,P.y0+P.H()*fraction+32*u};
        const bool hot=!m.profilePending && Hot(c,r,action,0);
        const ImU32 fill=loaded?kLime:(m.profilePending?kSurfaceHi:Rgba(80,34,42));
        Button(c,r,text,action,0,hot?Mix(fill,Rgba(255,255,255),0.12f):fill,loaded?Rgba(14,26,8):kText,!m.profilePending);
    };
    if (loaded && stage==3) button(0.50f,m.profile.wallpaper?"Skyline wallpaper":"iFruit wallpaper",Action::SetupWallpaper);
    if (loaded) for (unsigned i=0;i<4;++i) {
        const float radius=(i==stage?3.0f:2.0f)*u;
        c.dl->AddCircleFilled(V(P.Cx()+(static_cast<float>(i)-1.5f)*12*u,P.y0+P.H()*0.63f),radius,i<=stage?kLime:kText3,16);
    }
    button(0.69f,m.profilePending?"Saving...":loaded?(stage==3?"Finish setup":"Continue"):"Retry",loaded?Action::SetupNext:Action::ProfileRetry);
    Text(c,c.fb,c.L.fsSmall,P.Cx(),P.y0+P.H()*0.83f,kText3,"ESC closes · progress saves automatically",0.5f);
}

// Current screen, or both screens of a running transition. The deeper screen
// is always drawn on top.
void DrawScreens(Ctx& c) {
    ImDrawList* dl = c.dl;
    const Rect P = c.L.screen;
    const TransitionKind kind = c.c.Kind();
    if (kind == TransitionKind::None) { DrawPage(c, c.c.Current(), P); return; }

    const bool fwd = c.c.Forward();
    const Screen upper = fwd ? c.c.Current() : c.c.Previous();
    const Screen lower = fwd ? c.c.Previous() : c.c.Current();
    const float e = c.c.Transition();
    const float t = fwd ? e : 1.0f - e;               // 0 = lower shown, 1 = upper shown
    const ImVec2 mid(P.Cx(), P.Cy());

    if (kind == TransitionKind::Zoom) {
        int v0 = dl->VtxBuffer.Size;
        DrawPage(c, lower, P);
        Transform(dl, v0, mid, 1.0f + 0.06f * t, V(0, 0), 1.0f - 0.7f * t);
        const Rect icon = AppIconRect(c.L, upper.app);
        const float s = 0.16f + 0.84f * t;
        const ImVec2 out(icon.Cx() + (mid.x - icon.Cx()) * t, icon.Cy() + (mid.y - icon.Cy()) * t);
        const float hw = P.W() * 0.5f * s, hh = P.H() * 0.5f * s;
        dl->PushClipRect(V(out.x - hw, out.y - hh), V(out.x + hw, out.y + hh), true);
        v0 = dl->VtxBuffer.Size;
        DrawPage(c, upper, P);
        Transform(dl, v0, mid, s, V(out.x - mid.x, out.y - mid.y), (std::min)(1.0f, t * 1.8f));
        dl->PopClipRect();
    } else {
        int v0 = dl->VtxBuffer.Size;
        DrawPage(c, lower, P);
        Transform(dl, v0, mid, 1.0f, V(-0.28f * P.W() * t, 0), 1.0f);
        RoundRect(dl, P, Rgba(0, 0, 0, static_cast<int>(110 * t)), c.L.screenRadius);
        const float x = P.W() * (1.0f - t);
        const float r = c.L.screenRadius;
        for (int i = 1; i <= 6; ++i)
            dl->AddRectFilled(V(P.x0 + x - i * 2.0f * c.u, P.y0 + r), V(P.x0 + x, P.y1 - r), Rgba(0, 0, 0, 10));
        v0 = dl->VtxBuffer.Size;
        DrawPage(c, upper, P);
        Transform(dl, v0, mid, 1.0f, V(x, 0), 1.0f);
    }
}

void DrawCall(Ctx& c) {
    const float u = c.u;
    const Layout& L = c.L;
    const Rect P = L.screen;
    const Caller& who = c.m.caller;
    const int v0 = c.dl->VtxBuffer.Size;
    RoundRect(c.dl, P, Rgba(10, 16, 28), L.screenRadius);
    Gradient(c.dl, v0, P, Rgba(16, 26, 44), Rgba(4, 7, 12));

    const float ph = c.c.RingPhase(c.now);
    Text(c, c.fh, 9.0f * u, P.Cx(), L.content.y0 + 22.0f * u, kLime, "INCOMING CALL", 0.5f);
    const ImVec2 av(P.Cx(), L.content.y0 + 104.0f * u);
    const float r = 36.0f * u;
    for (int k = 0; k < 3; ++k) {
        const float q = std::fmod(ph + k / 3.0f, 1.0f);
        const float a = (1.0f - q) * (1.0f - q);
        c.dl->AddCircle(av, r + q * 40.0f * u, WithAlpha(kLime, 0.55f * a), 48, 1.6f * u);
    }
    c.dl->AddCircleFilled(av, r + 3.0f * u, Rgba(10, 16, 28), 40);
    Avatar(c, av, r, who.hue, who.name);
    Text(c, c.fh, 22.0f * u, P.Cx(), av.y + r + 26.0f * u, kText, who.name, 0.5f);
    Text(c, c.fb, L.fsBody, P.Cx(), av.y + r + 46.0f * u, kText2, DisplayNumber(who.number), 0.5f);
    Text(c, c.fb, L.fsSmall, P.Cx(), av.y + r + 62.0f * u, kText3, "SKR Mobile", 0.5f);

    const float by = L.content.y1 - 52.0f * u, br = 24.0f * u;
    const float bob = 1.0f + 0.05f * std::sin(ph * 2.0f * kPi);
    const ImVec2 dec(P.x0 + P.W() * 0.27f, by), acc(P.x0 + P.W() * 0.73f, by);
    const bool hd = Hot(c, {dec.x - br, dec.y - br, dec.x + br, dec.y + br}, Action::DeclineCall, 0);
    const bool ha = Hot(c, {acc.x - br, acc.y - br, acc.x + br, acc.y + br}, Action::AcceptCall, 0);
    c.dl->AddCircleFilled(dec, br, hd ? Mix(kCoral, Rgba(255, 255, 255), 0.15f) : kCoral, 36);
    GlyphHandset(c.dl, dec, 24.0f * u, Rgba(255, 255, 255), 0.75f * kPi);
    c.dl->AddCircleFilled(acc, br * bob, ha ? Mix(kLime, Rgba(255, 255, 255), 0.15f) : kLime, 36);
    GlyphHandset(c.dl, acc, 24.0f * u * bob, Rgba(14, 30, 8));
    Text(c, c.fb, L.fsSmall, dec.x, by + br + 12.0f * u, kText2, "Decline", 0.5f);
    Text(c, c.fb, L.fsSmall, acc.x, by + br + 12.0f * u, kText2, "Accept", 0.5f);
}

void DrawBanner(Ctx& c) {
    const float a = c.c.NotifyAmount(c.now);
    if (a <= 0.0f || !c.m.settings.banners) return;
    const float u = c.u;
    const Layout& L = c.L;
    const float h = 50.0f * u;
    const float y = L.statusBar.y1 + 2.0f * u - (1.0f - a) * (h + 30.0f * u);
    const Rect b{L.screen.x0 + 7.0f * u, y, L.screen.x1 - 7.0f * u, y + h};
    const int v0 = c.dl->VtxBuffer.Size;
    const bool tappable = c.m.banner.number[0] && a > 0.95f;
    const bool hov = tappable && Hot(c, b, Action::OpenBanner, 0);
    SoftShadow(c.dl, b.Offset(0, 3.0f * u), 14.0f * u, 8.0f * u, Rgba(0, 0, 0, 160));
    RoundRect(c.dl, b, hov ? Rgba(36, 46, 64, 250) : Rgba(28, 36, 50, 245), 14.0f * u);
    c.dl->AddRect(V(b.x0, b.y0), V(b.x1, b.y1), Rgba(255, 255, 255, 30), 14.0f * u, 0, 1.0f);
    const Rect ic{b.x0 + 9.0f * u, b.y0 + 9.0f * u, b.x0 + 31.0f * u, b.y0 + 31.0f * u};
    const AppDesc& d = Desc(c.m.banner.app);
    const int v1 = c.dl->VtxBuffer.Size;
    RoundRect(c.dl, ic, d.top, 6.0f * u);
    Gradient(c.dl, v1, ic, d.top, d.bottom);
    AppGlyph(c.dl, c.m.banner.app, V(ic.Cx(), ic.Cy()), ic.W() * 0.62f, Mix(d.top, d.bottom, 0.5f));
    Text(c, c.fh, 8.0f * u, ic.x1 + 8.0f * u, b.y0 + 13.0f * u, kText3, d.label);
    Text(c, c.fb, 8.5f * u, b.x1 - 10.0f * u, b.y0 + 13.0f * u, kText3, "now", 1.0f);
    char buf[64];
    const float tw = b.x1 - ic.x1 - 18.0f * u;
    Text(c, c.fh, L.fsBody * 0.95f, ic.x1 + 8.0f * u, b.y0 + 27.0f * u, kText, Fit(c, c.fh, L.fsBody * 0.95f, c.m.banner.title, tw, buf, sizeof(buf)));
    Text(c, c.fb, L.fsSmall, ic.x1 + 8.0f * u, b.y0 + 40.0f * u, kText2, Fit(c, c.fb, L.fsSmall, c.m.banner.text, tw, buf, sizeof(buf)));
    (void)v0;
}

void DrawStatusBar(Ctx& c) {
    const float u = c.u;
    const Layout& L = c.L;
    const float cy = L.island.Cy();
    Text(c, c.fh, L.fsStatus, L.screen.x0 + 22.0f * u, cy, kText, c.m.status.time);
    // Battery.
    const float bx1 = L.screen.x1 - 18.0f * u, bw = 17.0f * u, bh = 8.5f * u;
    const Rect bat{bx1 - bw, cy - bh * 0.5f, bx1, cy + bh * 0.5f};
    c.dl->AddRect(V(bat.x0, bat.y0), V(bat.x1, bat.y1), Rgba(240, 244, 248, 150), 2.5f * u, 0, 1.0f);
    RoundRect(c.dl, {bat.x1 + 1.0f * u, cy - 1.8f * u, bat.x1 + 2.4f * u, cy + 1.8f * u}, Rgba(240, 244, 248, 150), 1.0f * u);
    const float fill = std::clamp(c.m.status.battery / 100.0f, 0.0f, 1.0f);
    RoundRect(c.dl, {bat.x0 + 1.6f * u, bat.y0 + 1.6f * u, bat.x0 + 1.6f * u + (bw - 3.2f * u) * fill, bat.y1 - 1.6f * u},
              c.m.status.battery <= 20 ? kCoral : kText, 1.5f * u);
    // Signal bars.
    float sx = bat.x0 - 21.0f * u;
    for (int i = 0; i < 4; ++i) {
        const float h = (3.0f + 2.0f * i) * u;
        const Rect bar{sx + i * 4.0f * u, cy + 4.5f * u - h, sx + i * 4.0f * u + 2.8f * u, cy + 4.5f * u};
        RoundRect(c.dl, bar, i < c.m.status.signal ? kText : Rgba(240, 244, 248, 60), 0.8f * u);
    }
}

void DrawNavBar(Ctx& c) {
    const Rect& h = c.L.homeIndicator;
    const bool interactive = !c.c.CallIncoming();
    const Rect hit{c.L.navBar.x0 + 40.0f * c.u, c.L.navBar.y0, c.L.navBar.x1 - 40.0f * c.u, c.L.navBar.y1};
    const bool hov = interactive && Hot(c, hit, Action::Home, 0);
    RoundRect(c.dl, h, hov ? kLime : Rgba(240, 244, 248, 200), h.H() * 0.5f);
}

void DrawShell(Ctx& c) {
    ImDrawList* dl = c.dl;
    const Layout& L = c.L;
    const float u = c.u;
    // PHONE-M3 polish: ~half the ink of the M1 halo (peak ~0.44 vs ~0.70 opacity).
    SoftShadow(dl, L.shadow, L.bodyRadius, 16.0f * u, Rgba(0, 0, 0, 130));

    for (int i = 0; i < 3; ++i) {
        const Rect& b = L.sideButtons[i];
        const int v0 = dl->VtxBuffer.Size;
        RoundRect(dl, b, Rgba(46, 56, 72), 1.2f * u);
        Gradient(dl, v0, b, Rgba(70, 82, 102), Rgba(30, 36, 48));
    }
    // Lime accent on the power button: the SKR Street signature.
    const Rect& pw = L.sideButtons[0];
    RoundRect(dl, {pw.x1 - 1.4f * u, pw.y0 + 6.0f * u, pw.x1, pw.y1 - 6.0f * u}, WithAlpha(kLime, 0.9f), 0.7f * u);

    int v0 = dl->VtxBuffer.Size;
    RoundRect(dl, L.body, kBodyTop, L.bodyRadius);
    Gradient(dl, v0, L.body, kBodyTop, kBodyBottom);
    v0 = dl->VtxBuffer.Size;
    dl->AddRect(V(L.body.x0 + 0.5f, L.body.y0 + 0.5f), V(L.body.x1 - 0.5f, L.body.y1 - 0.5f), kRimLight, L.bodyRadius, 0, 1.0f);
    Gradient(dl, v0, L.body, kRimLight, kRimDark);
    const float rimR = L.bodyRadius - (L.frame.x0 - L.body.x0);
    dl->AddRect(V(L.frame.x0, L.frame.y0), V(L.frame.x1, L.frame.y1), Rgba(6, 8, 12, 220), rimR, 0, 1.0f);
    RoundRect(dl, L.frame.Inset(1.0f), kBezel, rimR - 1.0f);
    RoundRect(dl, L.speaker, Rgba(36, 44, 58), L.speaker.H() * 0.5f);
}

// Round the screen: bezel-coloured ring sectors between the screen radius and
// the frame's inner radius (both share the corner centre, Layout insets the
// screen by exactly the bezel). The square clip corner itself lies outside the
// device curve, so page backgrounds and bands are drawn rounded and nothing is
// ever painted there.
void DrawCornerMasks(Ctx& c) {
    const Rect& s = c.L.screen;
    const float r = c.L.screenRadius;
    const float R = c.L.bodyRadius - (c.L.frame.x0 - c.L.body.x0) - 0.5f;
    struct Corner { float cx, cy, a0; };
    const Corner corners[4] = {
        {s.x0 + r, s.y0 + r, kPi},
        {s.x1 - r, s.y0 + r, 1.5f * kPi},
        {s.x1 - r, s.y1 - r, 0.0f},
        {s.x0 + r, s.y1 - r, 0.5f * kPi},
    };
    // Convex quad strip without AA fringes (no seams between quads), then an
    // anti-aliased stroke along the inner arc for a smooth screen edge.
    // (AddConcavePolyFilled mis-triangulates this thin ring sector.)
    constexpr int kSeg = 14;
    const ImDrawListFlags saved = c.dl->Flags;
    for (const Corner& k : corners) {
        c.dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;
        for (int i = 0; i < kSeg; ++i) {
            const float t0 = k.a0 + 0.5f * kPi * i / kSeg, t1 = k.a0 + 0.5f * kPi * (i + 1) / kSeg;
            const ImVec2 q[4] = {V(k.cx + std::cos(t0) * r, k.cy + std::sin(t0) * r),
                                 V(k.cx + std::cos(t0) * R, k.cy + std::sin(t0) * R),
                                 V(k.cx + std::cos(t1) * R, k.cy + std::sin(t1) * R),
                                 V(k.cx + std::cos(t1) * r, k.cy + std::sin(t1) * r)};
            c.dl->AddConvexPolyFilled(q, 4, kBezel);
        }
        c.dl->Flags = saved;
        c.dl->PathArcTo(V(k.cx, k.cy), r + 0.3f, k.a0, k.a0 + 0.5f * kPi, kSeg);
        c.dl->PathStroke(kBezel, 0, 1.2f);
    }
    c.dl->AddRect(V(s.x0, s.y0), V(s.x1, s.y1), Rgba(255, 255, 255, 14), r, 0, 1.0f);
}

void DrawIsland(Ctx& c) {
    const Rect& i = c.L.island;
    RoundRect(c.dl, i, Rgba(0, 0, 0), i.H() * 0.5f);
    const ImVec2 lens(i.x1 - i.H() * 0.5f, i.Cy());
    c.dl->AddCircleFilled(lens, i.H() * 0.30f, Rgba(16, 24, 40), 16);
    c.dl->AddCircleFilled(lens, i.H() * 0.14f, Rgba(30, 52, 86), 12);
    c.dl->AddCircleFilled(V(lens.x - i.H() * 0.07f, lens.y - i.H() * 0.07f), i.H() * 0.05f, WithAlpha(kCyan, 0.8f), 6);
}

} // namespace

const Hit* FrameHits::At(float x, float y) const {
    for (int i = count - 1; i >= 0; --i)
        if (items[i].rect.Contains(x, y)) return &items[i];
    return nullptr;
}

void SetWallpaperTexture(void* texture, float width, float height) {
    g_wallTex = texture;
    g_wallW = width;
    g_wallH = height;
}

const char* AppLabel(AppId app) { return Desc(app).label; }

Rect AppIconRect(const Layout& L, AppId app) {
    int idx = 0;
    switch (app) {
        case AppId::Phone: idx = 0; break;
        case AppId::Messages: idx = 1; break;
        case AppId::Contacts: idx = 0; break;
        case AppId::Gps: idx = 1; break;
        case AppId::Settings: idx = 2; break;
        default: return {L.content.Cx() - 1, L.content.Cy() - 1, L.content.Cx() + 1, L.content.Cy() + 1};
    }
    const float u = L.u;
    const bool dock = app == AppId::Phone || app == AppId::Messages || app == AppId::Settings;
    const float size = dock ? 25.0f * u : 27.0f * u;
    const float pad = dock ? 17.0f * u : 0.0f;
    const int columns = dock ? 3 : 2;
    const float cell = (L.content.W() - 2.0f * pad) / static_cast<float>(columns);
    const float cx = L.content.x0 + pad + cell * (static_cast<float>(idx) + 0.5f);
    const float top = dock ? L.content.y1 - 38.0f * u : L.content.y0 + 171.0f * u;
    return {Snap(cx - size * 0.5f), Snap(top), Snap(cx + size * 0.5f), Snap(top + size)};
}

void Draw(ImDrawList* dl, const Layout& L, const Controller& c, const Model& m, Views& v,
          const Fonts& f, const Pointer& p, uint64_t now, FrameHits* hits) {
    if (!dl || !c.Visible() || !L.fits) return;
    if (hits) hits->count = 0;
    auto largest = [](ImFont* const* faces) {
        ImFont* best = nullptr;
        for (int i = 0; i < 3; ++i)
            if (faces[i] && (!best || faces[i]->FontSize > best->FontSize)) best = faces[i];
        return best ? best : ImGui::GetFont();
    };
    Ctx ctx{dl, L, c, m, v, &f, largest(f.heading), largest(f.body), p, now, hits,
            c.AcceptsClicks() && hits != nullptr, L.u};

    const int v0 = dl->VtxBuffer.Size;
    DrawShell(ctx);
    dl->PushClipRect(V(L.screen.x0, L.screen.y0), V(L.screen.x1, L.screen.y1), true);
    if (c.CallIncoming()) DrawCall(ctx);
    else if (m.setupSupported && (!m.profileLoaded || m.profile.stage<4)) DrawSetup(ctx);
    else DrawScreens(ctx);
    if (c.Current().app == AppId::Contacts && c.Current().page == 1 && v.contacts.confirmRemove &&
        !c.CallIncoming() && c.Kind() == TransitionKind::None)
        DrawRemoveConfirm(ctx);
    DrawNotice(ctx);
    if (!m.setupSupported || (m.profileLoaded && m.profile.stage==4)) DrawBanner(ctx);
    DrawStatusBar(ctx);
    DrawNavBar(ctx);
    dl->PopClipRect();
    DrawCornerMasks(ctx);
    DrawIsland(ctx);

    const float alpha = c.Alpha();
    if (alpha < 0.999f) Transform(dl, v0, V(0, 0), 1.0f, V(0, 0), alpha);
}

} // namespace skr::phone
