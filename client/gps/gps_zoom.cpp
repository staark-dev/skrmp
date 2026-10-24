#include "gps_zoom.h"
#include "gps_flags.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cmath>
#include <cstring>

namespace skr::gps::zoom {
namespace {

constexpr float kBand         = 0.04f;   // backlash: committed stays within ±4 % of the filtered target
constexpr float kFilterSec    = 0.5f;    // target low-pass (speed noise)
constexpr float kSmoothingSec = 0.9f;    // exponential time constant
constexpr float kMaxRatePerSec = 160.0f; // world units of range per second
constexpr float kReleaseEpsilon = 2.0f;
constexpr float kFastSpeed    = 40.0f;   // units/s (≈ 144 km/h) for full zoom-out

float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

} // namespace

float TargetRange(const Input& in) {
    const float s = Clamp(in.speed / kFastSpeed, 0.0f, 1.0f);
    const float eased = s * s * (3.0f - 2.0f * s);
    float target = 180.0f + (420.0f - 180.0f) * eased;
    // Keep an upcoming junction inside the view, but zoom in toward it for detail.
    if (in.junctionDistance > 0.0f && in.junctionDistance < 300.0f) {
        const float needed = in.junctionDistance * 1.35f + 40.0f;
        if (needed < target) target = needed;
    }
    return Clamp(target, kMinRange, kMaxRange);
}

float Controller::Update(const Input& in) {
    const float dt = Clamp(in.dt, 0.0f, 0.25f);
    if (!in.navigating) {
        if (!active_) return in.gtaRange;
        // Blend back to GTA's own range, then release.
        const float alpha = 1.0f - std::exp(-dt / kSmoothingSec);
        float delta = (in.gtaRange - current_) * alpha;
        const float maxStep = kMaxRatePerSec * dt;
        delta = Clamp(delta, -maxStep, maxStep);
        current_ += delta;
        if (std::fabs(current_ - in.gtaRange) <= kReleaseEpsilon) { active_ = false; return in.gtaRange; }
        return current_;
    }
    const float target = TargetRange(in);
    if (!active_) {
        active_ = true;
        current_ = Clamp(in.gtaRange, kMinRange, kMaxRange);
        committed_ = filtered_ = target;
    } else {
        // Low-pass the raw target, then a backlash (deadband) follower: the
        // committed target only moves when the filtered one pushes against
        // the ±kBand band, so small fluctuations never move it back and forth.
        filtered_ += (target - filtered_) * (1.0f - std::exp(-dt / kFilterSec));
        const float band = filtered_ * kBand;
        if (committed_ < filtered_ - band) committed_ = filtered_ - band;
        else if (committed_ > filtered_ + band) committed_ = filtered_ + band;
    }
    const float alpha = 1.0f - std::exp(-dt / kSmoothingSec);
    float delta = (committed_ - current_) * alpha;
    const float maxStep = kMaxRatePerSec * dt;
    current_ += Clamp(delta, -maxStep, maxStep);
    current_ = Clamp(current_, kMinRange, kMaxRange);
    return current_;
}

// ── GTA integration ─────────────────────────────────────────────────
namespace {

constexpr uintptr_t kCallSite       = 0x58A38A;   // CHud::DrawRadar → CRadar::DrawMap
constexpr uintptr_t kRadarDrawMap   = 0x586B00;
constexpr uintptr_t kRadarRange     = 0xBA8314;   // CRadar::m_radarRange
constexpr uintptr_t kFindPlayerSpeed = 0x56E090;  // CVector const& __cdecl(int)

Controller g_controller;
bool  g_installed = false;
bool  g_navigating = false;
float g_junction = -1.0f;
LARGE_INTEGER g_freq{}, g_last{};

bool ReadSpeed(float& out) {
    __try {
        const float* v = reinterpret_cast<const float* (__cdecl*)(int)>(kFindPlayerSpeed)(-1);
        if (!v) return false;
        out = std::sqrt(v[0] * v[0] + v[1] * v[1]) * 50.0f;   // per-step → per second
        return std::isfinite(out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void __cdecl DrawMapWrapper() {
    reinterpret_cast<void(__cdecl*)()>(kRadarDrawMap)();
    if (!flags::ZoomEnabled() && !g_controller.Active()) return;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const float dt = g_last.QuadPart && g_freq.QuadPart
        ? static_cast<float>(now.QuadPart - g_last.QuadPart) / static_cast<float>(g_freq.QuadPart) : 0.016f;
    g_last = now;

    float speed = 0.0f;
    ReadSpeed(speed);
    auto* range = reinterpret_cast<float*>(kRadarRange);
    Input in;
    in.gtaRange = *range;
    in.speed = speed;
    in.junctionDistance = g_junction;
    in.navigating = g_navigating && flags::ZoomEnabled();
    in.dt = dt;
    const float r = g_controller.Update(in);
    if (g_controller.Active() && std::isfinite(r)) *range = r;
}

bool PatchCall(void* target) {
    auto* site = reinterpret_cast<uint8_t*>(kCallSite);
    DWORD old = 0;
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(target) - (kCallSite + 5));
    memcpy(site + 1, &rel, 4);
    VirtualProtect(site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    return true;
}

uintptr_t CallTarget() {
    const auto* site = reinterpret_cast<const uint8_t*>(kCallSite);
    if (site[0] != 0xE8) return 0;
    int32_t rel = 0;
    memcpy(&rel, site + 1, 4);
    return kCallSite + 5 + static_cast<uintptr_t>(rel);
}

} // namespace

bool Install() {
    static bool attempted = false;
    if (attempted) return g_installed;
    attempted = true;
    QueryPerformanceFrequency(&g_freq);
    // Only a site still calling GTA's own DrawMap is patched (never over another hook).
    if (CallTarget() != kRadarDrawMap) {
        Log("zoom: 0x58A38A does not call 0x586B00 (target %p); adaptive zoom unavailable",
            reinterpret_cast<void*>(CallTarget()));
        return false;
    }
    g_installed = PatchCall(reinterpret_cast<void*>(&DrawMapWrapper));
    Log("zoom: call-site wrapper %s", g_installed ? "installed" : "FAILED");
    return g_installed;
}

void Uninstall() {
    if (!g_installed) return;
    if (CallTarget() == reinterpret_cast<uintptr_t>(&DrawMapWrapper))
        PatchCall(reinterpret_cast<void*>(kRadarDrawMap));
    g_installed = false;
    g_controller.Reset();
}

void SetNavigationContext(bool navigating, float junctionDistance) {
    g_navigating = navigating;
    g_junction = junctionDistance;
}

} // namespace skr::gps::zoom
