// API-003.2 — offscreen native preview of the GPS navigation panel and route
// strokes on a real Win32 Direct3D9 HAL device.
//
// PRODUCTION code drawn here (unchanged, linked from client/):
//   gps_draw.cpp       DrawRouteStrips + MinimapRouteStyle   (minimap route)
//   gps_clip.h         ClipPolyline                          (radar shape clip)
//   gps_panel.cpp      DrawPanel                             (navigation panel)
//   gps_panel_model.h  ComputeLayout, Model, FitText, ...    (panel layout/state)
//   map_route_draw.h   DrawRouteStrokes + StyleForScreen     (ESC MAP route)
//   gps_graph.cpp      LoadFromImg + FindRoute               (real gta3.img roads)
// EMULATED (no GTA process): the radar tiles (assets/minimap/minimap.png drawn
// through the same world→radar math as CRadar, round = ellipse at widescreen
// like GTA), the vitals strip, chat placeholder, player/waypoint blips, the
// ESC MAP frame, zone names (assets/minimap/zones.json + city rectangles
// from info.zon), and the world scene (flat backdrop).
//
// Usage: SKRGpsHudPreview <outDir> <minimap.png> <zones.json> [gta3.img]
// Writes PNGs + widths.txt. Exit 0 = all written.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <wincodec.h>

#include <imgui.h>
#include <backends/imgui_impl_dx9.h>

#include "gps/gps_clip.h"
#include "gps/gps_draw.h"
#include "gps/gps_graph.h"
#include "gps/gps_panel.h"
#include "gps/gps_panel_model.h"
#include "native-pause/map_route_draw.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace skr::gps;
namespace P = skr::gps::panel;
namespace G = skr::gps::graph;
namespace F = skr::gps::feedback;
namespace MR = skr::native_pause::map_route;

namespace {

IWICImagingFactory* g_wic = nullptr;
std::string g_out;
FILE* g_report = nullptr;

void Report(const char* fmt, ...) {
    va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a);
    if (g_report) { va_start(a, fmt); vfprintf(g_report, fmt, a); va_end(a); }
}

std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

// ── Image I/O (WIC) ────────────────────────────────────────────────────
bool LoadBgra(const std::string& path, std::vector<uint32_t>& px, UINT& w, UINT& h) {
    IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* fr = nullptr; IWICFormatConverter* cv = nullptr;
    bool ok = SUCCEEDED(g_wic->CreateDecoderFromFilename(Wide(path).c_str(), nullptr, GENERIC_READ,
                                                         WICDecodeMetadataCacheOnDemand, &dec)) &&
              SUCCEEDED(dec->GetFrame(0, &fr)) && SUCCEEDED(g_wic->CreateFormatConverter(&cv)) &&
              SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                       WICBitmapPaletteTypeCustom)) &&
              SUCCEEDED(cv->GetSize(&w, &h));
    if (ok) {
        px.resize(static_cast<size_t>(w) * h);
        ok = SUCCEEDED(cv->CopyPixels(nullptr, w * 4, w * h * 4, reinterpret_cast<BYTE*>(px.data())));
    }
    if (cv) cv->Release();
    if (fr) fr->Release();
    if (dec) dec->Release();
    return ok;
}

bool SavePng(const std::string& path, const uint32_t* px, UINT w, UINT h) {
    IWICStream* st = nullptr; IWICBitmapEncoder* enc = nullptr; IWICBitmapFrameEncode* fr = nullptr;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    bool ok = SUCCEEDED(g_wic->CreateStream(&st)) &&
              SUCCEEDED(st->InitializeFromFilename(Wide(path).c_str(), GENERIC_WRITE)) &&
              SUCCEEDED(g_wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
              SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) &&
              SUCCEEDED(enc->CreateNewFrame(&fr, nullptr)) && SUCCEEDED(fr->Initialize(nullptr)) &&
              SUCCEEDED(fr->SetSize(w, h)) && SUCCEEDED(fr->SetPixelFormat(&fmt)) &&
              SUCCEEDED(fr->WritePixels(h, w * 4, w * h * 4, reinterpret_cast<BYTE*>(const_cast<uint32_t*>(px)))) &&
              SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit());
    if (fr) fr->Release();
    if (enc) enc->Release();
    if (st) st->Release();
    return ok;
}

bool Capture(IDirect3DDevice9* dev, UINT w, UINT h, std::vector<uint32_t>& px) {
    IDirect3DSurface9* bb = nullptr; IDirect3DSurface9* sys = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return false;
    D3DSURFACE_DESC d; bb->GetDesc(&d);
    bool ok = SUCCEEDED(dev->CreateOffscreenPlainSurface(w, h, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) &&
              SUCCEEDED(dev->GetRenderTargetData(bb, sys));
    D3DLOCKED_RECT lr{};
    if (ok && SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
        px.resize(static_cast<size_t>(w) * h);
        for (UINT y = 0; y < h; ++y) {
            const uint32_t* row = reinterpret_cast<const uint32_t*>(static_cast<const BYTE*>(lr.pBits) + y * lr.Pitch);
            for (UINT x = 0; x < w; ++x) px[y * w + x] = row[x] | 0xFF000000u;
        }
        sys->UnlockRect();
    } else ok = false;
    if (sys) sys->Release();
    bb->Release();
    return ok;
}

// Nearest-neighbour magnified crop (pixel-exact stroke inspection).
void SaveCrop(const std::string& path, const std::vector<uint32_t>& px, UINT w, int x0, int y0, int cw, int ch, int k) {
    std::vector<uint32_t> out(static_cast<size_t>(cw * k) * ch * k);
    for (int y = 0; y < ch * k; ++y)
        for (int x = 0; x < cw * k; ++x)
            out[static_cast<size_t>(y) * cw * k + x] = px[static_cast<size_t>(y0 + y / k) * w + (x0 + x / k)];
    SavePng(path, out.data(), cw * k, ch * k);
}

// ── Zones (emulated ResolveZones) ──────────────────────────────────────
struct Zone { std::string name; float x1, y1, x2, y2; };
std::vector<Zone> g_zones;

void LoadZones(const std::string& path) {
    std::ifstream f(path); std::stringstream ss; ss << f.rdbuf();
    const std::string s = ss.str();
    size_t at = 0;
    auto num = [&](const char* key, size_t from) {
        const size_t k = s.find(key, from);
        return std::stof(s.substr(s.find(':', k) + 1));
    };
    while ((at = s.find("\"name\"", at)) != std::string::npos) {
        const size_t q0 = s.find('"', s.find(':', at) + 1), q1 = s.find('"', q0 + 1);
        Zone z{s.substr(q0 + 1, q1 - q0 - 1), num("\"x1\"", at), num("\"y1\"", at), num("\"x2\"", at), num("\"y2\"", at)};
        g_zones.push_back(z);
        at = q1;
    }
}

void ResolveZones(float x, float y, char* area, size_t al, char* city, size_t cl) {
    // City rectangles from GTA's info.zon (z2 = 900 rule in game_state.cpp).
    struct C { const char* n; float x1, y1, x2, y2; };
    static const C kCities[] = {{"Los Santos", 44.6f, -2892.9f, 2997.0f, -768.0f},
                                {"San Fierro", -2997.4f, -1115.5f, -1213.9f, 1659.6f},
                                {"Las Venturas", 869.4f, 596.3f, 2997.0f, 2993.8f}};
    area[0] = city[0] = '\0';
    float best = 1e30f;
    for (const Zone& z : g_zones)
        if (x >= z.x1 && x <= z.x2 && y >= z.y1 && y <= z.y2) {
            const float a = (z.x2 - z.x1) * (z.y2 - z.y1);
            if (a < best) { best = a; snprintf(area, al, "%s", z.name.c_str()); }
        }
    for (const C& c : kCities)
        if (x >= c.x1 && x <= c.x2 && y >= c.y1 && y <= c.y2) snprintf(city, cl, "%s", c.n);
}

// ── Routes from the real road graph ────────────────────────────────────
G::RoadGraph g_graph;
bool g_graphOk = false;

std::vector<Vec3> RoutePoints(Vec3 a, Vec3 b) {
    std::vector<Vec3> pts;
    if (!g_graphOk) return pts;
    const G::SearchResult r = G::FindRoute(g_graph, a, b);
    if (r.status != G::SearchStatus::Found) return pts;
    pts.push_back(r.start);
    for (uint32_t n : r.path) pts.push_back({g_graph.nodes[n].x, g_graph.nodes[n].y, g_graph.nodes[n].z});
    pts.push_back(r.end);
    return pts;
}

float Length(const std::vector<Vec3>& p, size_t from = 0) {
    float l = 0;
    for (size_t i = from + 1; i < p.size(); ++i) l += std::hypot(p[i].x - p[i - 1].x, p[i].y - p[i - 1].y);
    return l;
}

// Direction of travel ~40 units ahead (GTA's radar turns with the camera,
// which follows the vehicle): the route leaves the player upwards.
float HeadingAhead(const std::vector<Vec3>& p) {
    float acc = 0;
    for (size_t i = 1; i < p.size(); ++i) {
        acc += std::hypot(p[i].x - p[i - 1].x, p[i].y - p[i - 1].y);
        if (acc >= 40.0f || i + 1 == p.size()) return std::atan2(p[i].y - p[0].y, p[i].x - p[0].x);
    }
    return 0.0f;
}

// Player on the route where the remaining length = `remaining` (or the start).
// Returns the route from the player onward; heading = route direction there.
std::vector<Vec3> FromRemaining(const std::vector<Vec3>& route, float remaining, Vec3& player, float& heading) {
    float acc = 0;
    for (size_t i = route.size() - 1; i > 0; --i) {
        const Vec3 a = route[i - 1], b = route[i];
        const float seg = std::hypot(b.x - a.x, b.y - a.y);
        if (acc + seg >= remaining || i == 1) {
            const float t = seg > 0 ? std::clamp(1.0f - (remaining - acc) / seg, 0.0f, 1.0f) : 0.0f;
            player = {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z};
            heading = std::atan2(b.y - a.y, b.x - a.x);
            std::vector<Vec3> rest{player};
            rest.insert(rest.end(), route.begin() + i, route.end());
            heading = HeadingAhead(rest);
            return rest;
        }
        acc += seg;
    }
    player = route.front(); heading = 0;
    return route;
}

// ── Emulated radar (CRadar math) ───────────────────────────────────────
struct Radar {
    float x, y, w, h;      // screen rect (measured rect in game)
    bool  square;
    Vec3  player;
    float heading;         // radians, world direction shown "up"
    float range;           // world units from centre to edge
};

clip::P2 WorldToRadar(const Radar& r, float wx, float wy) {
    const float dx = (wx - r.player.x) / r.range, dy = (wy - r.player.y) / r.range;
    const float rot = 1.5707963f - r.heading;      // heading → +y
    return {dx * std::cos(rot) - dy * std::sin(rot), dx * std::sin(rot) + dy * std::cos(rot)};
}
ScreenPoint RadarToScreen(const Radar& r, clip::P2 p) {
    return {r.x + (p.x + 1.0f) * 0.5f * r.w, r.y + (1.0f - p.y) * 0.5f * r.h};
}

struct TexVertex { float x, y, z, rhw; DWORD c; float u, v; };
struct ColVertex { float x, y, z, rhw; DWORD c; };

void DrawRadarMap(IDirect3DDevice9* dev, IDirect3DTexture9* map, const Radar& r) {
    std::vector<TexVertex> v;
    auto vert = [&](clip::P2 rp) {
        const float rot = 1.5707963f - r.heading;
        const float wx = r.player.x + (rp.x * std::cos(-rot) - rp.y * std::sin(-rot)) * r.range;
        const float wy = r.player.y + (rp.x * std::sin(-rot) + rp.y * std::cos(-rot)) * r.range;
        const ScreenPoint s = RadarToScreen(r, rp);
        return TexVertex{s.x - 0.5f, s.y - 0.5f, 0, 1, 0xFFFFFFFF, (wx + 3000.0f) / 6000.0f, (3000.0f - wy) / 6000.0f};
    };
    if (r.square) {
        v = {vert({-1, 1}), vert({1, 1}), vert({1, -1}), vert({-1, -1})};
    } else {
        v.push_back(vert({0, 0}));
        for (int i = 0; i <= 96; ++i) {
            const float a = i * 6.2831853f / 96;
            v.push_back(vert({std::cos(a), std::sin(a)}));
        }
    }
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    dev->SetTexture(0, map);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetVertexShader(nullptr);
    dev->SetPixelShader(nullptr);
    const HRESULT hr = dev->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, static_cast<UINT>(v.size() - 2), v.data(), sizeof(TexVertex));
    static bool once = false;
    if (!once) { once = true; Report("  radar map draw hr=0x%08lX verts=%zu uv0=(%.3f,%.3f)\n", hr, v.size(), v[0].u, v[0].v); }
    dev->SetTexture(0, nullptr);
}

void FillRect(IDirect3DDevice9* dev, float x0, float y0, float x1, float y1, DWORD c) {
    const ColVertex v[4] = {{x0, y0, 0, 1, c}, {x1, y0, 0, 1, c}, {x0, y1, 0, 1, c}, {x1, y1, 0, 1, c}};
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(ColVertex));
}

// Production minimap route path (gps_renderer.cpp RenderRoute, minus GTA calls).
void DrawMinimapRoute(IDirect3DDevice9* dev, const Radar& r, const std::vector<Vec3>& route, const RouteStyle& style) {
    std::vector<clip::P2> radar, clipped;
    std::vector<uint32_t> counts;
    for (const Vec3& p : route) radar.push_back(WorldToRadar(r, p.x, p.y));
    clip::ClipPolyline(radar.data(), radar.size(), r.square ? clip::Region::UnitSquare : clip::Region::UnitCircle,
                       clipped, counts);
    if (counts.empty()) return;
    std::vector<ScreenPoint> screen;
    for (const clip::P2& p : clipped) screen.push_back(RadarToScreen(r, p));
    RECT sc{static_cast<LONG>(r.x), static_cast<LONG>(r.y), static_cast<LONG>(std::ceil(r.x + r.w)),
            static_cast<LONG>(std::ceil(r.y + r.h))};
    DrawRouteStrips(dev, screen.data(), counts.data(), counts.size(), r.square ? nullptr : &sc, style);
}

// ── Scenario ───────────────────────────────────────────────────────────
struct Scene {
    const char* name;
    bool  customRadar = false;     // narrow square custom radar
    Vec3  from, to;
    float remaining = -1;          // place the player at this remaining distance
    float range = 220;
    F::Summary::State state = F::Summary::State::Navigating;
    bool  escMap = false;
    float mapZoomFit = 6.0f;       // ESC MAP zoom (× whole-map fit)
    bool  crop = false;
    P::Seat seat = P::Seat::Driver;      // seat while the route is set up
    P::Seat seatAfter = P::Seat::Driver; // seat after the change (transition scenes)
    int   afterMs = -1;                  // capture this long after the seat change
};

struct Frame {
    UINT W, H;
    IDirect3DDevice9* dev;
    IDirect3DTexture9* map;
    ImFont* heading;
    ImFont* body;
};

Radar RadarFor(const Frame& f, bool custom) {
    const float sx = f.W / 640.0f, sy = f.H / 448.0f;
    Radar r{};
    if (custom) {   // Square/Custom: e.g. server custom size 70 × 90 HUD units
        r.x = 40.0f * sx; r.w = 70.0f * sx; r.h = 90.0f * sy; r.y = f.H - 130.0f * sy; r.square = true;
    } else {        // GTA default slot (CHud::DrawRadar constants)
        r.x = 40.0f * sx; r.w = 94.0f * sx; r.h = 76.0f * sy; r.y = f.H - 116.0f * sy; r.square = false;
    }
    return r;
}

void DrawBackdrop(const Frame& f) {
    // Flat dusk tone (no GTA scene in an offscreen preview) + chat placeholder.
    const float W = static_cast<float>(f.W), H = static_cast<float>(f.H);
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    bg->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(W, H), IM_COL32(84, 98, 112, 255), IM_COL32(84, 98, 112, 255),
                                IM_COL32(58, 62, 58, 255), IM_COL32(58, 62, 58, 255));
    const float fs = std::round(13.0f * H / 768.0f);
    static const char* kChat[] = {"SA-MP 0.3.7-R5 Started", "Connected to SKR Runtime Server 1.0.23",
                                  "[Server] Welcome to SKR Enhanced", "[GPS] Waypoint set from the map",
                                  "Player_One: on my way", "Player_Two: meet at the High Roller",
                                  "[Server] Preview chat placeholder", "Player_One: ok",
                                  "[Server] 10 chat lines shown", "Player_Two: see you there"};
    for (int i = 0; i < 10; ++i)
        bg->AddText(f.body, fs, ImVec2(std::round(28.0f * W / 1360.0f), std::round(10.0f * H / 768.0f + i * fs * 1.25f)),
                    IM_COL32(235, 235, 235, 230), kChat[i]);
    bg->AddText(f.body, fs, ImVec2(W - 330.0f * W / 1360.0f, H - fs * 1.6f), IM_COL32(255, 255, 255, 120),
                "offscreen D3D9 preview - world scene not rendered");
}

void DrawVitalsAndBlips(const Frame& f, const Radar& r, const Vec3& waypoint) {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const float sy = f.H / 448.0f;
    // radar_hud v3: one strip under the radar, health left / armour right.
    const float top = std::round(r.y + r.h + 2.0f * sy), th = std::max(2.0f, std::round(2.5f * sy));
    const float mid = r.x + r.w * 0.5f, g = std::max(1.0f, std::round(1.5f * f.W / 640.0f) * 0.5f);
    dl->AddRectFilled(ImVec2(r.x, top), ImVec2(mid - g, top + th), IM_COL32(168, 238, 101, 255));
    dl->AddRectFilled(ImVec2(mid + g, top), ImVec2(r.x + r.w, top + th), IM_COL32(92, 140, 182, 255));
    if (r.square) dl->AddRect(ImVec2(r.x - 1, r.y - 1), ImVec2(r.x + r.w + 1, r.y + r.h + 1), IM_COL32(0, 0, 0, 90));
    else dl->AddEllipse(ImVec2(r.x + r.w * 0.5f, r.y + r.h * 0.5f), ImVec2(r.w * 0.5f + 0.5f, r.h * 0.5f + 0.5f),
                        IM_COL32(0, 0, 0, 110), 0.0f, 96, 1.5f);
    // Player arrow (points up: radar is heading-up) and waypoint blip.
    const float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f, s = 5.0f * f.H / 768.0f;
    dl->AddTriangleFilled(ImVec2(cx, cy - s * 1.4f), ImVec2(cx - s, cy + s), ImVec2(cx + s, cy + s), IM_COL32(255, 255, 255, 255));
    dl->AddTriangle(ImVec2(cx, cy - s * 1.4f), ImVec2(cx - s, cy + s), ImVec2(cx + s, cy + s), IM_COL32(0, 0, 0, 200));
    const clip::P2 wp = WorldToRadar(r, waypoint.x, waypoint.y);
    const bool inside = r.square ? std::fabs(wp.x) <= 1 && std::fabs(wp.y) <= 1 : wp.x * wp.x + wp.y * wp.y <= 1;
    if (inside) {
        const ScreenPoint p = RadarToScreen(r, wp);
        dl->AddCircleFilled(ImVec2(p.x, p.y), s * 0.9f, IM_COL32(235, 70, 90, 255));
        dl->AddCircle(ImVec2(p.x, p.y), s * 0.9f, IM_COL32(0, 0, 0, 200), 0, 1.0f);
    }
}

bool RenderScene(const Frame& f, const Scene& sc, const char* tag, const RouteStyle* overrideStyle = nullptr) {
    IDirect3DDevice9* dev = f.dev;
    std::vector<Vec3> full = RoutePoints(sc.from, sc.to);
    if (full.size() < 2) { Report("  %s: no route\n", sc.name); return false; }
    Vec3 player{}; float heading = 0;
    std::vector<Vec3> route = sc.remaining > 0 ? FromRemaining(full, sc.remaining, player, heading) : full;
    if (sc.remaining <= 0) { player = full.front(); heading = HeadingAhead(full); }
    const float remaining = Length(route);
    static int lenReported = 0;
    if (lenReported++ < 9) Report("  [%s] route %.0f m total, %.0f m remaining\n", sc.name, Length(full), remaining);

    // Panel state through the production model (same calls as gps_panel.cpp).
    P::Model model;
    char area[64], city[64];
    ResolveZones(player.x, player.y, area, sizeof(area), city, sizeof(city));
    model.SetLocation(area, city);
    // Seat-aware context exactly as gps_panel.cpp builds it: on foot the box
    // shows the straight-line (2D) distance to the waypoint.
    P::NavContext ctx;
    ctx.seat = sc.seat;
    ctx.waypointDistance = std::hypot(sc.to.x - player.x, sc.to.y - player.y);
    F::Summary s; s.state = F::Summary::State::Navigating; s.remaining = remaining; s.stateSince = 1;
    model.Update(s, ctx, 1);                  // establishes the distance
    s.state = sc.state;
    if (sc.state == F::Summary::State::Arrived) { ctx.hasWaypoint = false; ctx.waypointDistance = -1; }
    uint32_t now = 1 + P::kFadeMs + 10;
    model.Update(s, ctx, now);                // settled (no fade)
    if (sc.afterMs >= 0) {                    // seat change, 60 fps ticks
        ctx.seat = sc.seatAfter;
        for (int e = 0; e <= sc.afterMs; e += 16) model.Update(s, ctx, now + 16 + e);
    }

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(f.W), static_cast<float>(f.H));
    io.DeltaTime = 1.0f / 60.0f;

    dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF202830, 1.0f, 0);
    dev->BeginScene();
    Radar r = RadarFor(f, sc.customRadar);
    r.player = player; r.heading = heading; r.range = sc.range;

    ImGui_ImplDX9_NewFrame();
    ImGui::NewFrame();
    if (!sc.escMap) {
        DrawBackdrop(f);
        ImGui::EndFrame(); ImGui::Render(); ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
        // GTA HUD pass: radar tiles, route (production), vitals + blips.
        DrawRadarMap(dev, f.map, r);
        DrawMinimapRoute(dev, r, route, overrideStyle ? *overrideStyle : MinimapRouteStyle(static_cast<float>(f.H)));
        ImGui_ImplDX9_NewFrame(); ImGui::NewFrame();
        DrawVitalsAndBlips(f, r, sc.to);
        // EndScene pass: production panel above the measured radar rect.
        P::LayoutInput in;
        in.radar = {r.x, r.y, r.x + r.w, r.y + r.h};
        in.screenW = static_cast<float>(f.W); in.screenH = static_cast<float>(f.H); in.uiScale = 1.0f;
        const P::Layout L = P::ComputeLayout(in);
        P::DrawPanel(ImGui::GetForegroundDrawList(), model.GetView(), L, f.heading, f.body);
        // On-foot / passenger box: production layout + draw (as gps_panel.cpp DrawImGui).
        const P::CompactView& cv = model.GetCompact();
        P::CompactLayout CL;
        if (cv.visible) {
            const P::CompactLayout probe = P::ComputeCompactLayout(in, !r.square, 0.0f);
            const float textW = f.heading->CalcTextSizeA(probe.font, FLT_MAX, 0.0f, cv.value).x;
            CL = P::ComputeCompactLayout(in, !r.square, textW);
            P::DrawCompact(ImGui::GetForegroundDrawList(), cv, CL, f.heading);
        }
        static const char* kModes[] = {"hidden", "full", "compact"};
        Report("  %-34s %ux%u mode %-7s | panel %s a=%.2f %s %s | compact %s a=%.2f %s box %.0fx%.0f @(%.0f,%.0f)\n",
               sc.name, f.W, f.H, kModes[static_cast<int>(model.Mode())], model.GetView().visible ? "on " : "off",
               model.GetView().alpha, model.GetView().label, model.GetView().value, cv.visible ? "on " : "off", cv.alpha,
               cv.value, CL.box.Width(), CL.box.Height(), CL.box.x0, CL.box.y0);
    } else {
        // ESC MAP frame (emulated chrome) + production route strokes.
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        const float W = static_cast<float>(f.W), H = static_cast<float>(f.H);
        dl->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), IM_COL32(9, 14, 22, 255));
        const ImVec2 a(std::round(W * 0.06f), std::round(H * 0.16f)), b(std::round(W * 0.94f), std::round(H * 0.93f));
        dl->AddText(f.heading, std::round(28.0f * H / 1080.0f), ImVec2(a.x, std::round(H * 0.07f)), IM_COL32(240, 244, 246, 255), "MAP");
        const float fit = std::max(b.x - a.x, b.y - a.y) / 6000.0f, zoom = fit * sc.mapZoomFit;
        float cx = 0, cy = 0;
        for (const Vec3& p : full) { cx += p.x; cy += p.y; }
        cx /= full.size(); cy /= full.size();
        auto w2s = [&](float wx, float wy) {
            return ImVec2((a.x + b.x) * 0.5f + (wx - cx) * zoom, (a.y + b.y) * 0.5f - (wy - cy) * zoom);
        };
        dl->PushClipRect(a, b, true);
        const ImVec2 m0 = w2s(-3000, 3000), m1 = w2s(3000, -3000);
        dl->AddImage(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(f.map)), m0, m1);
        std::vector<ImVec2> pts;
        for (const Vec3& p : route) pts.push_back(w2s(p.x, p.y));
        const float travelled = Length(full) - remaining;
        MR::DrawRouteStrokes(dl, pts.data(), static_cast<int>(pts.size()), travelled * zoom, a, b,
                             w2s(full.back().x, full.back().y), w2s(sc.to.x, sc.to.y), MR::StyleForScreen(H));
        const ImVec2 wp = w2s(sc.to.x, sc.to.y), pp = w2s(player.x, player.y);
        const float s5 = 6.0f * H / 768.0f;
        dl->AddCircleFilled(wp, s5, IM_COL32(235, 70, 90, 255));
        dl->AddCircle(wp, s5, IM_COL32(0, 0, 0, 200), 0, 1.5f);
        dl->AddCircleFilled(pp, s5 * 0.8f, IM_COL32(255, 255, 255, 255));
        dl->PopClipRect();
        dl->AddRect(a, b, IM_COL32(255, 255, 255, 40));
        Report("  %-34s %ux%u zoom %.1fx fit, %zu route pts, dash %.2f px underlay %.2f px\n", sc.name, f.W, f.H,
               sc.mapZoomFit, pts.size(), MR::StyleForScreen(H).dashWidth, MR::StyleForScreen(H).underlayWidth);
    }
    ImGui::EndFrame(); ImGui::Render(); ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
    dev->EndScene();

    std::vector<uint32_t> px;
    if (!Capture(dev, f.W, f.H, px)) return false;
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\%s_%ux%u.png", g_out.c_str(), tag, f.W, f.H);
    bool ok = SavePng(path, px.data(), f.W, f.H);
    if (!sc.escMap) {   // radar + panel region, 3x
        const int x0 = std::max(0, static_cast<int>(r.x) - 12), y0 = std::max(0, static_cast<int>(r.y - 70.0f * f.H / 768.0f));
        const int cw = std::min(static_cast<int>(r.w) + 24, static_cast<int>(f.W) - x0);
        const int ch = std::min(static_cast<int>(r.y + r.h - y0) + 20, static_cast<int>(f.H) - y0);
        snprintf(path, sizeof(path), "%s\\%s_%ux%u_zoom3x.png", g_out.c_str(), tag, f.W, f.H);
        SaveCrop(path, px, f.W, x0, y0, cw, ch, 3);
    } else if (sc.crop) {
        const int cw = static_cast<int>(f.W / 4), ch = static_cast<int>(f.H / 4);
        snprintf(path, sizeof(path), "%s\\%s_%ux%u_zoom3x.png", g_out.c_str(), tag, f.W, f.H);
        SaveCrop(path, px, f.W, static_cast<int>(f.W / 2 - cw / 2), static_cast<int>(f.H / 2 - ch / 2), cw, ch, 3);
    }
    return ok;
}

// ── Measured stroke widths ─────────────────────────────────────────────
// Opaque white stroke on black, horizontal and 30° lines: effective width =
// Σ intensity / 255 along a perpendicular pixel column (× cos for slanted).
float MeasureMinimap(IDirect3DDevice9* dev, UINT W, UINT H, float width, float angleDeg) {
    RouteStyle st{width, width, 0xFFFFFFFF, 0x00000000};
    const float a = angleDeg * 3.14159265f / 180.0f;
    const ScreenPoint pts[] = {{200.0f, 300.3f}, {200.0f + 400.0f * std::cos(a), 300.3f + 400.0f * std::sin(a)}};
    const uint32_t n = 2;
    dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
    dev->BeginScene();
    DrawRouteStrips(dev, pts, &n, 1, nullptr, st);
    dev->EndScene();
    std::vector<uint32_t> px;
    Capture(dev, W, H, px);
    const int x = 300;
    const float yc = 300.3f + (x - 200) * std::tan(a);
    float sum = 0;
    for (int y = static_cast<int>(yc) - 20; y <= static_cast<int>(yc) + 20; ++y) sum += (px[y * W + x] & 0xFF) / 255.0f;
    return sum * std::cos(a);
}

float MeasureImGui(IDirect3DDevice9* dev, UINT W, UINT H, float thickness) {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(W), static_cast<float>(H));
    dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
    dev->BeginScene();
    ImGui_ImplDX9_NewFrame(); ImGui::NewFrame();
    const ImVec2 pts[] = {ImVec2(200, 300.3f), ImVec2(600, 300.3f)};
    ImGui::GetBackgroundDrawList()->AddPolyline(pts, 2, IM_COL32(255, 255, 255, 255), ImDrawFlags_None, thickness);
    ImGui::EndFrame(); ImGui::Render(); ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
    dev->EndScene();
    std::vector<uint32_t> px;
    Capture(dev, W, H, px);
    float sum = 0;
    for (int y = 280; y <= 320; ++y) sum += (px[y * W + 300] & 0xFF) / 255.0f;
    return sum;
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

} // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 4) { std::puts("usage: SKRGpsHudPreview <outDir> <minimap.png> <zones.json> [gta3.img]"); return 2; }
    g_out = argv[1];
    CreateDirectoryA(g_out.c_str(), nullptr);
    g_report = fopen((g_out + "\\widths.txt").c_str(), "w");
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_wic)))) return 3;
    LoadZones(argv[3]);
    std::wstring img = L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Grand Theft Auto San Andreas\\models\\gta3.img";
    if (argc > 4) img = Wide(argv[4]);
    std::string err;
    g_graphOk = G::LoadFromImg(img, g_graph, err);
    Report("road graph: %s (%zu nodes) zones: %zu\n", g_graphOk ? "loaded" : err.c_str(), g_graph.nodes.size(), g_zones.size());
    std::vector<uint32_t> mapPx; UINT mw = 0, mh = 0;
    if (!LoadBgra(argv[2], mapPx, mw, mh)) { Report("minimap.png not loaded\n"); return 4; }

    // Alignment check of the backdrop image against real road nodes: fraction
    // of vehicle nodes on dark (road) pixels for small offsets / scales.
    if (g_graphOk) {
        auto dark = [&](float wx, float wy, float sc, float dx, float dy) {
            const float u = ((wx + dx) * sc + 3000.0f) / 6000.0f, v = (3000.0f - (wy + dy) * sc) / 6000.0f;
            const int px = static_cast<int>(u * mw), py = static_cast<int>(v * mh);
            if (px < 0 || py < 0 || px >= static_cast<int>(mw) || py >= static_cast<int>(mh)) return false;
            const uint32_t c = mapPx[static_cast<size_t>(py) * mw + px];
            return ((c >> 16) & 0xFF) < 60 && ((c >> 8) & 0xFF) < 60 && (c & 0xFF) < 60;
        };
        float best = -1, bs = 1, bx = 0, by = 0, base = 0;
        for (float sc : {0.97f, 0.98f, 0.99f, 1.0f, 1.01f, 1.02f, 1.03f})
            for (float dx = -60; dx <= 60; dx += 4)
                for (float dy = -60; dy <= 60; dy += 4) {
                    int hit = 0, n = 0;
                    for (size_t i = 0; i < g_graph.nodes.size(); i += 1) {
                        const G::Node& nd = g_graph.nodes[i];
                        if (!nd.vehicle || nd.water) continue;
                        if (nd.x < 1400 || nd.x > 2700 || nd.y < -1950 || nd.y > -1150) continue;   // central LS
                        ++n; hit += dark(nd.x, nd.y, sc, dx, dy);
                    }
                    const float f = n ? static_cast<float>(hit) / n : 0;
                    if (sc == 1.0f && dx == 0 && dy == 0) base = f;
                    if (f > best) { best = f; bs = sc; bx = dx; by = dy; }
                }
        Report("backdrop alignment (central LS road nodes): identity %.1f%% of road nodes on road pixels; best %.1f%% at scale %.2f offset (%.0f,%.0f)\n",
               base * 100, best * 100, bs, bx, by);
    }

    WNDCLASSA wc{}; wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleA(nullptr); wc.lpszClassName = "SKRGpsPreview";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA(wc.lpszClassName, "SKR GPS preview", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!hwnd || !d3d) return 5;

    // Real routes (gta3.img road graph) through recognisable areas.
    const Vec3 kStrip = {2040.0f, 1520.0f, 10.7f}, kHighRoller = {1940.0f, 1390.0f, 9.0f};
    const Vec3 kLvAirportN = {1500.0f, 1900.0f, 10.8f};
    const Vec3 kGrove = {2495.0f, -1665.0f, 13.3f}, kDowntown = {1560.0f, -1300.0f, 17.0f};
    const Vec3 kLsAirport = {1690.0f, -2420.0f, 13.5f}, kIdlewood = {1990.0f, -1740.0f, 13.5f};
    const Vec3 kFromNorthLv = {2400.0f, 2400.0f, 10.8f};
    const Vec3 kRandolph = {1700.0f, 700.0f, 10.8f};
    const Vec3 kEastBeach = {2800.0f, -1400.0f, 20.0f}, kRodeo = {-20.0f, -1500.0f, 15.0f};
    Scene scenes[] = {
        {"1_standard_minimap"},
        {"2_custom_narrow_radar"},
        {"3_long_location_name"},
        {"4_short_distance_290m"},
        {"5_long_distance_3_3km"},
        {"6_recalculating"},
        {"7_thin_route_city_road"},
        {"8_escmap_dashed_route"},
        {"9_driving_full_panel"},
        {"10_walking_compact"},
        {"11_passenger_compact"},
        {"12_walking_custom_radar"},
        {"13a_exit_vehicle_150ms"},
        {"13b_exit_vehicle_380ms"},
        {"13c_exit_vehicle_560ms"},
        {"13d_exit_vehicle_720ms"},
        {"14_arrival_driving"},
        {"15_arrival_on_foot"},
    };
    scenes[0].from = kGrove;     scenes[0].to = kDowntown;   scenes[0].remaining = 1800;
    scenes[1].from = kGrove;     scenes[1].to = kDowntown;   scenes[1].remaining = 1800; scenes[1].customRadar = true;
    scenes[2].from = kRandolph;  scenes[2].to = kHighRoller; scenes[2].remaining = -1; scenes[2].customRadar = true;
    scenes[3].from = kFromNorthLv; scenes[3].to = kHighRoller; scenes[3].remaining = 290; scenes[3].range = 180;
    scenes[4].from = kEastBeach; scenes[4].to = kRodeo;      scenes[4].remaining = 3300;
    scenes[5].from = kGrove;     scenes[5].to = kDowntown;   scenes[5].remaining = 1500; scenes[5].state = F::Summary::State::Recalculating;
    scenes[6].from = kDowntown;  scenes[6].to = kGrove;      scenes[6].remaining = 2200; scenes[6].range = 140;
    scenes[7].from = kGrove;     scenes[7].to = kDowntown;   scenes[7].remaining = 2400; scenes[7].escMap = true; scenes[7].mapZoomFit = 7.0f; scenes[7].crop = true;
    using St = P::Seat;
    for (int i = 8; i <= 17; ++i) { scenes[i].from = kGrove; scenes[i].to = kDowntown; scenes[i].remaining = 1500; }
    scenes[8].seat = St::Driver;
    scenes[9].seat = St::OnFoot;     scenes[9].range = 180;
    scenes[10].seat = St::Passenger;
    scenes[11].seat = St::OnFoot;    scenes[11].customRadar = true; scenes[11].range = 180;
    const int exitMs[] = {150, 380, 560, 720};
    for (int i = 0; i < 4; ++i) { scenes[12 + i].seat = St::Driver; scenes[12 + i].seatAfter = St::OnFoot; scenes[12 + i].afterMs = exitMs[i]; }
    scenes[16].seat = St::Driver;    scenes[16].state = F::Summary::State::Arrived; scenes[16].remaining = 20;
    scenes[17].seat = St::OnFoot;    scenes[17].state = F::Summary::State::Arrived; scenes[17].remaining = 20;
    (void)kStrip; (void)kLvAirportN; (void)kIdlewood; (void)kLsAirport;

    int written = 0, failed = 0;
    const UINT sizes[][2] = {{1360, 768}, {1920, 1080}};
    for (const auto& sz : sizes) {
        D3DPRESENT_PARAMETERS pp{};
        pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferFormat = D3DFMT_UNKNOWN;
        pp.BackBufferWidth = sz[0]; pp.BackBufferHeight = sz[1]; pp.hDeviceWindow = hwnd;
        IDirect3DDevice9* dev = nullptr;
        if (FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev))) {
            Report("CreateDevice %ux%u failed\n", sz[0], sz[1]); ++failed; continue;
        }
        IDirect3DTexture9* map = nullptr;
        const HRESULT thr = dev->CreateTexture(mw, mh, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &map, nullptr);
        Report("map texture %ux%u hr=0x%08lX first px=0x%08X\n", mw, mh, thr, mapPx[mw * mh / 2 + mw / 2]);
        D3DLOCKED_RECT lr{};
        map->LockRect(0, &lr, nullptr, 0);
        for (UINT y = 0; y < mh; ++y) memcpy(static_cast<BYTE*>(lr.pBits) + y * lr.Pitch, &mapPx[y * mw], mw * 4);
        map->UnlockRect(0);

        // ImGui exactly as native-pause Initialize loads its faces (40 px, OS 2x1).
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.Fonts->AddFontDefault();
        static const ImWchar kRanges[] = {0x0020, 0x00FF, 0x0100, 0x017F, 0x0218, 0x021B, 0};
        auto load = [&](const char* file) -> ImFont* {
            char p[MAX_PATH]; char dir[MAX_PATH]; GetWindowsDirectoryA(dir, MAX_PATH);
            snprintf(p, sizeof(p), "%s\\Fonts\\%s", dir, file);
            if (GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES) return nullptr;
            ImFontConfig cfg; cfg.OversampleH = 2; cfg.OversampleV = 1;
            return io.Fonts->AddFontFromFileTTF(p, 40.0f, &cfg, kRanges);
        };
        ImFont* heading = load("bahnschrift.ttf");
        if (!heading) heading = load("arialbd.ttf");
        ImFont* body = load("segoeui.ttf");
        if (!body) body = load("arial.ttf");
        ImGui_ImplDX9_Init(dev);
        Frame f{sz[0], sz[1], dev, map, heading, body};

        Report("\n== %ux%u ==\n", sz[0], sz[1]);
        for (const Scene& sc : scenes) {
            if (RenderScene(f, sc, sc.name)) ++written; else ++failed;
        }
        // Before/after on the same city road (old widths 7 / 11 px, old colours).
        const RouteStyle oldStyle{7.0f, 11.0f, 0xCC77FF44, 0xCC0A1A0A};
        if (RenderScene(f, scenes[6], "7b_BEFORE_old_thick_route", &oldStyle)) ++written; else ++failed;

        // Measured widths (rendered pixels).
        const RouteStyle now = MinimapRouteStyle(static_cast<float>(sz[1]));
        Report("measured minimap fill   : nominal %.2f -> %.2f px (0 deg), %.2f px (30 deg)\n", now.fillWidth,
               MeasureMinimap(dev, sz[0], sz[1], now.fillWidth, 0), MeasureMinimap(dev, sz[0], sz[1], now.fillWidth, 30));
        Report("measured minimap outline: nominal %.2f -> %.2f px (0 deg)\n", now.outlineWidth,
               MeasureMinimap(dev, sz[0], sz[1], now.outlineWidth, 0));
        Report("measured OLD fill/outline (7.0 / 11.0 nominal): %.2f / %.2f px\n",
               MeasureMinimap(dev, sz[0], sz[1], 7.0f, 0), MeasureMinimap(dev, sz[0], sz[1], 11.0f, 0));
        const MR::MapRouteStyle ms = MR::StyleForScreen(static_cast<float>(sz[1]));
        const float oldScale = sz[1] / 1080.0f;   // old Scale() at UI scale 1.0
        Report("measured ESC dash/underlay: nominal %.2f/%.2f -> %.2f/%.2f px; OLD %.2f/%.2f -> %.2f/%.2f px\n",
               ms.dashWidth, ms.underlayWidth, MeasureImGui(dev, sz[0], sz[1], ms.dashWidth),
               MeasureImGui(dev, sz[0], sz[1], ms.underlayWidth), 3.5f * oldScale, 7.0f * oldScale,
               MeasureImGui(dev, sz[0], sz[1], 3.5f * oldScale), MeasureImGui(dev, sz[0], sz[1], 7.0f * oldScale));

        ImGui_ImplDX9_Shutdown();
        ImGui::DestroyContext();
        map->Release();
        dev->Release();
    }
    d3d->Release();
    DestroyWindow(hwnd);
    Report("\nwritten %d, failed %d\n", written, failed);
    if (g_report) fclose(g_report);
    g_wic->Release();
    CoUninitialize();
    return failed ? 1 : 0;
}
