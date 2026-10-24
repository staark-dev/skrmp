#pragma once
// ═══════════════════════════════════════════════════════════════════════
// Adaptive GPS radar zoom (navigation view range)
// ═══════════════════════════════════════════════════════════════════════
// GTA computes CRadar::m_radarRange (0xBA8314) every frame in CRadar::DrawMap
// (0x586B00: 180 on foot / slow, up to 340–350 with speed), called from
// CHud::DrawRadar at 0x58A38A. Map tiles, blips and the GPS route are drawn
// afterwards with that range. While navigating (and when enabled in
// gps_debug.ini, AdaptiveZoom=1), the call site is wrapped so the range is
// replaced after GTA computed it. Radar geometry (screen rect, masks) is
// untouched; GTA recomputes its own range every frame, so nothing needs
// restoring when navigation ends.

#include <cstdint>

namespace skr::gps::zoom {

constexpr float kMinRange = 160.0f;
constexpr float kMaxRange = 460.0f;

struct Input {
    float gtaRange = 180.0f;     // GTA's own range this frame
    float speed = 0.0f;          // world units per second
    float junctionDistance = -1; // along-route distance to the next maneuver, <0 = none
    bool  navigating = false;
    float dt = 0.016f;           // seconds since the previous frame
};

// Pure controller: smoothing, hysteresis, rate limit, clamps.
class Controller {
public:
    float Update(const Input& in);   // range to use this frame
    bool  Active() const { return active_; }
    float Target() const { return committed_; }
    void  Reset() { active_ = false; current_ = committed_ = filtered_ = 0.0f; }

private:
    bool  active_ = false;     // currently overriding (or blending back)
    float current_ = 0.0f;
    float committed_ = 0.0f;   // backlash-filtered target
    float filtered_ = 0.0f;    // low-passed raw target
};

// Target before smoothing (exposed for tests).
float TargetRange(const Input& in);

// Installs the 0x58A38A call-site wrapper (only if it still calls 0x586B00).
bool Install();
void Uninstall();
// Called by the engine with the latest navigation context (GTA thread).
void SetNavigationContext(bool navigating, float junctionDistance);

} // namespace skr::gps::zoom
