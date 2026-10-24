#pragma once
// ═══════════════════════════════════════════════════════════════════════
// GPS HUD display policy — which navigation widget is shown (API-003.2)
// ═══════════════════════════════════════════════════════════════════════
// Pure and deterministic (unit-tested in test_gps_panel.cpp). Inputs come
// from the game tick (seat, from CPed/CVehicle state) and the engine summary;
// the panel model only asks Current().
//
//   driver seat            → Full    (panel above the minimap)
//   on foot / passenger    → Compact (distance-only box on the radar)
//   no waypoint            → Hidden
//
// A change between Full and Compact must hold for kSwitchDelayMs before it
// is applied, so the enter/exit animations and a single odd sample never
// make the widgets flicker. Showing from Hidden and hiding are immediate.
// SetOverride() is the hook for the GPS Pawn API (API-003.3).

#include <cstdint>

namespace skr::gps::panel {

enum class Seat : uint8_t { OnFoot, Driver, Passenger };
enum class DisplayMode : uint8_t { Hidden, Full, Compact };
enum class ModeOverride : uint8_t { Auto, ForceFull, ForceCompact, ForceHidden };

constexpr uint32_t kSwitchDelayMs = 300;

inline DisplayMode DesiredMode(bool navigationActive, Seat seat, ModeOverride o) {
    if (!navigationActive || o == ModeOverride::ForceHidden) return DisplayMode::Hidden;
    if (o == ModeOverride::ForceFull)    return DisplayMode::Full;
    if (o == ModeOverride::ForceCompact) return DisplayMode::Compact;
    return seat == Seat::Driver ? DisplayMode::Full : DisplayMode::Compact;
}

class DisplayPolicy {
public:
    void Reset() { *this = DisplayPolicy(); }
    void SetOverride(ModeOverride o) { m_override = o; }
    ModeOverride Override() const { return m_override; }
    DisplayMode Current() const { return m_current; }

    DisplayMode Update(bool navigationActive, Seat seat, uint32_t now) {
        const DisplayMode want = DesiredMode(navigationActive, seat, m_override);
        if (want == m_current) { m_pending = m_current; return m_current; }
        if (want == DisplayMode::Hidden || m_current == DisplayMode::Hidden) {
            m_current = m_pending = want;              // show / hide at once
            return m_current;
        }
        if (m_pending != want) { m_pending = want; m_pendingSince = now; }
        else if (now - m_pendingSince >= kSwitchDelayMs) m_current = want;
        return m_current;
    }

private:
    DisplayMode  m_current = DisplayMode::Hidden;
    DisplayMode  m_pending = DisplayMode::Hidden;
    uint32_t     m_pendingSince = 0;
    ModeOverride m_override = ModeOverride::Auto;
};

} // namespace skr::gps::panel
