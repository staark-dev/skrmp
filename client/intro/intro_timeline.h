#pragma once
#include <algorithm>
#include <cmath>

// Normative recipe from SKR-URBAN-INTRO-IMPLEMENTATION-PROMPT.md §3.
// All rectangles are on the 1920x1080 composition canvas, origin top-left.

namespace skr::intro {

constexpr float kCanvasW = 1920.0f;
constexpr float kCanvasH = 1080.0f;
constexpr float kDuration = 9.0f;
constexpr float kSkipAfter = 0.7f;
// Until the player is in the world the picture holds here and music keeps playing;
// the final fades (audio 8.1 -> 9.0, picture 8.25 -> 9.0) run once the world is ready.
constexpr float kHoldAt = 8.1f;
// Longest hold before fading out anyway (server down / connection failing), so the
// SA-MP connection messages become visible.
constexpr float kMaxHold = 20.0f;
constexpr float kBgLastFrame = 269.0f;

inline float Clamp01(float x) { return (std::min)((std::max)(x, 0.0f), 1.0f); }
inline float Fade(float t, float a, float d) { return Clamp01((t - a) / d); }
inline float EaseOutCubic(float x) { float y = 1.0f - Clamp01(x); return 1.0f - y * y * y; }

struct Rect { float x, y, w, h; };
struct Uv { float u0, v0, u1, v1; };

struct Layer {
    Rect rect;
    Uv uv;
    float alpha;
};

// Reference frame index; continuous so the zoom stays smooth above 30 FPS.
// At integer frame times it equals clamp(floor(t*30), 0, 269).
inline float BackgroundFrame(float t) { return (std::min)((std::max)(t * 30.0f, 0.0f), kBgLastFrame); }

inline Layer Background(float t) {
    float n = BackgroundFrame(t);
    float z = 1.055f - 0.025f * n / kBgLastFrame;
    float u0 = 0.5f - 0.5f / z + 20.0f * std::sin(n / kBgLastFrame) / 3840.0f;
    float v0 = 0.5f - 0.5f / z;
    return {{0, 0, kCanvasW, kCanvasH}, {u0, v0, u0 + 1.0f / z, v0 + 1.0f / z}, 1.0f};
}

inline Layer Foreground(float t) {
    float x = -60.0f + 42.0f * EaseOutCubic(t / 1.6f) + 10.0f * t / 9.0f;
    return {{x, -20.0f, 1980.0f, 1114.0f}, {0, 0, 1, 1}, Fade(t, 0.35f, 1.05f)};
}

inline Layer Logo(float t) {
    float c = 1.0f - Clamp01((t - 1.25f) / 0.8f);
    float q = c * c * c;
    return {{850.0f + 48.0f * q, 205.0f + 16.0f * q, 1000.0f, 342.0f}, {0, 0, 1, 1}, Fade(t, 1.25f, 0.70f)};
}

constexpr unsigned kFooterRgb = 0x20211C;
inline Layer Footer(float t) {
    return {{0, 948.0f, 1920.0f, 132.0f}, {0, 0, 1, 1}, Fade(t, 2.45f, 0.60f)};
}

struct TextLayer { const wchar_t* file; Rect rect; float start; float duration; };
constexpr TextLayer kTextLayers[] = {
    {L"subtitle.png",  {1189, 585, 322, 24}, 1.90f, 0.60f},
    {L"prototype.png", {1241, 652, 217, 16}, 2.15f, 0.60f},
    {L"line1.png",     {546, 986, 828, 28},  2.60f, 0.60f},
    {L"line2.png",     {624, 1029, 672, 25}, 2.70f, 0.60f},
};

inline Layer Text(const TextLayer& l, float t) {
    return {l.rect, {0, 0, 1, 1}, Fade(t, l.start, l.duration)};
}

// Overall fade multiplies composition RGB (layer alpha untouched); the matte stays opaque.
inline float MasterFade(float t) {
    return (std::min)(Fade(t, 0.0f, 0.45f), 1.0f - Fade(t, 8.25f, 0.75f));
}

// Audio envelope: volume 0.70, fade-in 0.8 s on the playback clock, fade-out 8.1 -> 9.0 s
// on the intro (picture) time, silence afterwards. Without a hold both clocks are equal.
inline float AudioGain(float playback, float introTime) {
    if (introTime >= kDuration || playback < 0.0f) return 0.0f;
    return 0.70f * (std::min)(Clamp01(playback / 0.8f), 1.0f - Clamp01((introTime - 8.1f) / 0.9f));
}
inline float AudioGain(float t) { return AudioGain(t, t); }

// Aspect-preserving placement of the canvas inside the back buffer.
struct Placement { float s, ox, oy; };
inline Placement Place(float backW, float backH) {
    float s = (std::min)(backW / kCanvasW, backH / kCanvasH);
    return {s, (backW - kCanvasW * s) / 2.0f, (backH - kCanvasH * s) / 2.0f};
}

// Loading bar (not part of the reference clip; added on request). Right side of the
// footer on line2's row (line2 ends at x=1296), 30 % of the canvas wide, HUD cyan
// #74DCE8 on a faint track; it follows the footer's fade-in and the master fade.
constexpr Rect kBarRect = {1320.0f, 1037.5f, 576.0f, 8.0f};
constexpr unsigned kBarRgb = 0x74DCE8;
constexpr float kGtaLoadShare = 0.8f;
// GTA 1.0 US counts 140 loading chunks (gta-reversed CLoadingScreen::NewChunkLoaded).
constexpr int kGtaLoadingChunks = 140;

// Overall progress: GTA chunks fill 80 %, joining the server fills the rest.
inline float LoadProgress(int chunksLoaded, bool connected) {
    float gta = Clamp01(static_cast<float>(chunksLoaded) / kGtaLoadingChunks);
    return connected ? 1.0f : kGtaLoadShare * gta;
}

} // namespace skr::intro
