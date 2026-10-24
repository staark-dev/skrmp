#include "gps_draw.h"

#include <cmath>
#include <vector>

namespace skr::gps {
namespace {

// Muted lime #A3E65B at 88 %, dark separation #102016 at 55 %.
constexpr DWORD kRouteLime   = 0xE0A3E65B;  // ARGB
constexpr DWORD kOutlineDark = 0x8C102016;  // ARGB

struct Vertex { float x, y, z, rhw; DWORD color; };
constexpr DWORD kVertexFVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;

void Quad(std::vector<Vertex>& v, ScreenPoint a, DWORD ca, ScreenPoint b, DWORD cb,
          ScreenPoint c, DWORD cc, ScreenPoint d, DWORD cd) {
    // a-b is the edge at point i, c-d the matching edge at point i+1.
    v.push_back({a.x, a.y, 0.0f, 1.0f, ca});
    v.push_back({b.x, b.y, 0.0f, 1.0f, cb});
    v.push_back({c.x, c.y, 0.0f, 1.0f, cc});
    v.push_back({b.x, b.y, 0.0f, 1.0f, cb});
    v.push_back({d.x, d.y, 0.0f, 1.0f, cd});
    v.push_back({c.x, c.y, 0.0f, 1.0f, cc});
}

// One anti-aliased stroke as a triangle list: solid core between the inner
// rails, alpha fading to zero across kFringe px on each side.
void AppendStroke(const ScreenPoint* pts, size_t n, float width, DWORD color, std::vector<Vertex>& verts) {
    static std::vector<ScreenPoint> inner, outer;
    float innerHalf = (width - kFringe) * 0.5f;
    if (innerHalf < 0.25f) innerHalf = 0.25f;
    BuildStripOutline(pts, n, innerHalf, inner);
    BuildStripOutline(pts, n, innerHalf + kFringe, outer);
    if (inner.size() < 4 || inner.size() != outer.size()) return;
    const DWORD clear = color & 0x00FFFFFF;
    for (size_t i = 0; i + 3 < inner.size(); i += 2) {
        const ScreenPoint iL0 = inner[i], iR0 = inner[i + 1], iL1 = inner[i + 2], iR1 = inner[i + 3];
        const ScreenPoint oL0 = outer[i], oR0 = outer[i + 1], oL1 = outer[i + 2], oR1 = outer[i + 3];
        Quad(verts, oL0, clear, iL0, color, oL1, clear, iL1, color);
        Quad(verts, iL0, color, iR0, color, iL1, color, iR1, color);
        Quad(verts, iR0, color, oR0, clear, iR1, color, oR1, clear);
    }
}

} // namespace

RouteStyle MinimapRouteStyle(float screenHeight) {
    float k = screenHeight > 0.0f ? screenHeight / 768.0f : 1.0f;
    if (k < 1.0f) k = 1.0f;
    if (k > 1.15f) k = 1.15f;
    RouteStyle s;
    s.fillWidth    = 2.0f * k;
    s.outlineWidth = s.fillWidth + 0.9f;
    s.fillColor    = kRouteLime;
    s.outlineColor = kOutlineDark;
    return s;
}

void BuildStripOutline(const ScreenPoint* pts, size_t n, float halfW, std::vector<ScreenPoint>& out) {
    out.clear();
    std::vector<ScreenPoint> p;
    p.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        if (!p.empty()) {
            const float dx = pts[i].x - p.back().x, dy = pts[i].y - p.back().y;
            if (dx * dx + dy * dy < 0.25f) continue;
        }
        p.push_back(pts[i]);
    }
    if (p.size() < 2) return;
    auto normal = [&](size_t a, size_t b) {
        const float dx = p[b].x - p[a].x, dy = p[b].y - p[a].y;
        const float len = std::sqrt(dx * dx + dy * dy);
        return ScreenPoint{-dy / len, dx / len};
    };
    auto emit = [&](const ScreenPoint& at, const ScreenPoint& dir, float scale) {
        out.push_back({at.x + dir.x * scale, at.y + dir.y * scale});
        out.push_back({at.x - dir.x * scale, at.y - dir.y * scale});
    };
    out.reserve(p.size() * 2 + 8);
    for (size_t i = 0; i < p.size(); ++i) {
        if (i == 0)              { emit(p[0], normal(0, 1), halfW); continue; }
        if (i + 1 == p.size())   { emit(p[i], normal(i - 1, i), halfW); continue; }
        const ScreenPoint n0 = normal(i - 1, i), n1 = normal(i, i + 1);
        const float mx = n0.x + n1.x, my = n0.y + n1.y;
        const float ml = std::sqrt(mx * mx + my * my);
        // cos(half turn angle) = ml / 2; miter length = halfW / cosHalf.
        const float cosHalf = ml * 0.5f;
        if (cosHalf * kMiterLimit > 1.0f) {
            emit(p[i], ScreenPoint{mx / ml, my / ml}, halfW / cosHalf);
        } else {
            // Bevel: close the outer corner with the two segment ends.
            emit(p[i], n0, halfW);
            emit(p[i], n1, halfW);
        }
    }
}

bool DrawRouteStrip(IDirect3DDevice9* dev, const ScreenPoint* pts, size_t count,
                    const RECT* clip) {
    const uint32_t n = static_cast<uint32_t>(count);
    return count >= 2 && DrawRouteStrips(dev, pts, &n, 1, clip);
}

bool DrawRouteStrips(IDirect3DDevice9* dev, const ScreenPoint* pts, const uint32_t* counts,
                     size_t pieces, const RECT* clip) {
    return DrawRouteStrips(dev, pts, counts, pieces, clip, MinimapRouteStyle(768.0f));
}

bool DrawRouteStrips(IDirect3DDevice9* dev, const ScreenPoint* pts, const uint32_t* counts,
                     size_t pieces, const RECT* clip, const RouteStyle& style) {
    if (!dev || !pts || !counts || pieces == 0) return false;

    // All outlines first, then all fills, so fills sit above every outline.
    static std::vector<Vertex> outline, fill;
    outline.clear();
    fill.clear();
    size_t offset = 0;
    for (size_t k = 0; k < pieces; ++k) {
        if (counts[k] >= 2) {
            AppendStroke(pts + offset, counts[k], style.outlineWidth, style.outlineColor, outline);
            AppendStroke(pts + offset, counts[k], style.fillWidth, style.fillColor, fill);
        }
        offset += counts[k];
    }
    if (fill.size() < 3) return false;

    IDirect3DStateBlock9* saved = nullptr;
    if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &saved)) || !saved) return false;

    // Fixed-function, untextured, alpha-blended screen-space triangles.
    dev->SetVertexShader(nullptr);
    dev->SetPixelShader(nullptr);
    dev->SetFVF(kVertexFVF);
    dev->SetTexture(0, nullptr);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
    if (clip) {
        dev->SetScissorRect(clip);
        dev->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
    }

    // DrawPrimitiveUP leaves stream 0 unbound; the state block puts it back.
    bool ok = true;
    if (outline.size() >= 3)
        ok = SUCCEEDED(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, static_cast<UINT>(outline.size() / 3),
                                            outline.data(), sizeof(Vertex)));
    ok = SUCCEEDED(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, static_cast<UINT>(fill.size() / 3),
                                        fill.data(), sizeof(Vertex))) && ok;

    const bool restored = SUCCEEDED(saved->Apply());
    saved->Release();
    return ok && restored;
}

} // namespace skr::gps
