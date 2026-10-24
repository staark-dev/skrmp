#pragma once
// PHONE-001 — SKR Street Phone (Concept B): measured device layout.
//
// Pure geometry, no game or ImGui state. Every rectangle the renderer uses is
// derived here from the backbuffer size and the UI scale, so no coordinates
// are scattered through the drawing code.
//
//   unit u      = backbuffer height / 768 × UI scale (applied once)
//   body        = 400u × 196u (≈ 9 : 18.4 screen), anchored bottom-right
//   reserved    = top 30 % of the screen (GTA clock / weapon / money / vitals)
//   ultrawide   = anchored to the right edge of a centred 21:9 region, so the
//                 phone stays near the player's focus on 32:9 screens
//
// The body shrinks (never moves into the reserved band) when the scaled phone
// would not fit; slide offsets for open/close animations are applied by the
// caller through LayoutInput::slide.

#include <algorithm>
#include <cmath>

namespace skr::phone {

struct Rect {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    float W() const { return x1 - x0; }
    float H() const { return y1 - y0; }
    float Cx() const { return (x0 + x1) * 0.5f; }
    float Cy() const { return (y0 + y1) * 0.5f; }
    bool Contains(float x, float y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
    Rect Inset(float d) const { return {x0 + d, y0 + d, x1 - d, y1 - d}; }
    Rect Offset(float dx, float dy) const { return {x0 + dx, y0 + dy, x1 + dx, y1 + dy}; }
};

constexpr float kMinUiScale = 0.75f;
constexpr float kMaxUiScale = 1.35f;
constexpr float kReservedTop = 0.30f;     // GTA top-right HUD band
constexpr float kMaxAspect = 21.0f / 9.0f;

struct LayoutInput {
    float screenW = 1360, screenH = 768;
    float uiScale = 1.0f;     // future Settings value, clamped to [0.75, 1.35]
    float slide = 1.0f;       // 1 = resting on screen, 0 = fully below the bottom edge
};

struct Layout {
    float u = 1;              // effective unit after fit
    Rect shadow;              // soft drop-shadow base
    Rect body;                // outer device body
    float bodyRadius = 0;
    Rect frame;               // inner rim (body minus metal edge)
    Rect screen;              // display region: ALL app content is clipped here
    float screenRadius = 0;
    Rect island;              // camera / speaker pill inside the status bar
    Rect speaker;             // earpiece slit in the top bezel
    Rect statusBar;           // time, signal, battery
    Rect content;             // app page area (screen minus status bar and nav)
    Rect navBar;              // gesture indicator strip
    Rect homeIndicator;
    Rect sideButtons[3];      // [0] power (right), [1] volume up, [2] volume down (left)
    // Text sizes in pixels (fonts are rasterised at 40 px, scaled at draw).
    float fsStatus = 0, fsTitle = 0, fsBody = 0, fsSmall = 0, fsClock = 0;
    bool fits = true;         // false when the screen is too small for a usable phone
};

inline float Snap(float v) { return std::round(v); }

inline Layout ComputeLayout(const LayoutInput& in) {
    Layout L;
    const float W = (std::max)(in.screenW, 1.0f), H = (std::max)(in.screenH, 1.0f);
    const float scale = std::clamp(in.uiScale, kMinUiScale, kMaxUiScale);
    float u = H / 768.0f * scale;

    const float marginR = 28.0f, marginB = 18.0f;   // in units
    const float bodyHu = 400.0f, bodyWu = 196.0f;
    // Fit: the body must stay below the reserved band.
    const float maxBodyH = H * (1.0f - kReservedTop) - marginB * u;
    if (bodyHu * u > maxBodyH) u = maxBodyH / (bodyHu + marginB);
    L.u = u;
    L.fits = bodyHu * u >= 220.0f;                  // < 220 px tall is unreadable

    const float regionW = (std::min)(W, H * kMaxAspect);
    const float right = Snap((W + regionW) * 0.5f - marginR * u);
    const float bodyH = Snap(bodyHu * u), bodyW = Snap(bodyWu * u);
    const float restTop = Snap(H - marginB * u - bodyH);
    const float hiddenTop = H + 24.0f * u;          // shadow included
    const float slide = std::clamp(in.slide, 0.0f, 1.0f);
    const float top = Snap(hiddenTop + (restTop - hiddenTop) * slide);

    L.body = {right - bodyW, top, right, top + bodyH};
    L.bodyRadius = Snap(30.0f * u);
    L.shadow = L.body.Offset(0, 4.0f * u);
    L.frame = L.body.Inset((std::max)(1.0f, Snap(2.0f * u)));

    const float bezel = (std::max)(3.0f, Snap(6.0f * u));
    L.screen = L.body.Inset(bezel);
    L.screenRadius = (std::max)(4.0f, L.bodyRadius - bezel);

    const float statusH = Snap(26.0f * u), navH = Snap(18.0f * u);
    L.statusBar = {L.screen.x0, L.screen.y0, L.screen.x1, L.screen.y0 + statusH};
    L.navBar = {L.screen.x0, L.screen.y1 - navH, L.screen.x1, L.screen.y1};
    L.content = {L.screen.x0, L.statusBar.y1, L.screen.x1, L.navBar.y0};

    const float islandW = Snap(54.0f * u), islandH = Snap(15.0f * u);
    const float icx = L.screen.Cx();
    L.island = {Snap(icx - islandW * 0.5f), L.screen.y0 + Snap(6.0f * u),
                Snap(icx + islandW * 0.5f), L.screen.y0 + Snap(6.0f * u) + islandH};
    const float spkW = Snap(30.0f * u), spkH = (std::max)(1.0f, Snap(1.6f * u));
    L.speaker = {Snap(icx - spkW * 0.5f), L.body.y0 + Snap(2.4f * u),
                 Snap(icx + spkW * 0.5f), L.body.y0 + Snap(2.4f * u) + spkH};

    const float indW = Snap(62.0f * u), indH = (std::max)(2.0f, Snap(3.2f * u));
    L.homeIndicator = {Snap(icx - indW * 0.5f), Snap(L.navBar.Cy() - indH * 0.5f),
                       Snap(icx + indW * 0.5f), Snap(L.navBar.Cy() - indH * 0.5f) + indH};

    const float btnD = (std::max)(2.0f, Snap(2.2f * u));
    L.sideButtons[0] = {L.body.x1 - 1, L.body.y0 + Snap(92 * u), L.body.x1 + btnD, L.body.y0 + Snap(140 * u)};
    L.sideButtons[1] = {L.body.x0 - btnD, L.body.y0 + Snap(78 * u), L.body.x0 + 1, L.body.y0 + Snap(108 * u)};
    L.sideButtons[2] = {L.body.x0 - btnD, L.body.y0 + Snap(116 * u), L.body.x0 + 1, L.body.y0 + Snap(146 * u)};

    L.fsStatus = 11.2f * u;
    L.fsTitle = 19.0f * u;
    L.fsBody = 14.0f * u;
    L.fsSmall = 11.4f * u;
    L.fsClock = 42.0f * u;
    return L;
}

} // namespace skr::phone
