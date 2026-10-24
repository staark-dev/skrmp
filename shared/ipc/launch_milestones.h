#pragma once
// Launcher ↔ client readiness milestones (one launch session).
//
//   CLIENT_INITIALIZED  SkrInitialize accepted (client DLL up)
//   SAMP_LOADED         D3D9 + SA-MP hooks installed, samp.dll identified
//   SAMP_READY          SA-MP created CNetGame (runtime structures usable)
//   GAMEPLAY_CONFIRMED  connected, player in world and healthy (not Present-dependent)
//   VISUAL_READY        cover dismissed after a successfully presented frame
//   SESSION_READY       launcher: handoff done and (VISUAL_READY or GAMEPLAY_CONFIRMED)
//
// SAMP_READY alone never certifies a healthy session. Each milestone is
// recorded once per session; repeats are duplicates, other sessions are stale.

#include <cstdint>

namespace skr::ipc {

enum class Milestone : uint8_t {
    ClientInitialized = 0,
    SampLoaded,
    SampReady,
    GameplayConfirmed,
    VisualReady,
    SessionReady,
    Count
};
constexpr int kMilestoneCount = static_cast<int>(Milestone::Count);

inline const char* MilestoneName(Milestone m) {
    switch (m) {
        case Milestone::ClientInitialized: return "CLIENT_INITIALIZED";
        case Milestone::SampLoaded:        return "SAMP_LOADED";
        case Milestone::SampReady:         return "SAMP_READY";
        case Milestone::GameplayConfirmed: return "GAMEPLAY_CONFIRMED";
        case Milestone::VisualReady:       return "VISUAL_READY";
        case Milestone::SessionReady:      return "SESSION_READY";
        default:                           return "?";
    }
}

// Milestones that must precede `m` in a correct launch.
inline bool PrerequisitesMet(Milestone m, const bool (&has)[kMilestoneCount]) {
    auto h = [&](Milestone x) { return has[static_cast<int>(x)]; };
    switch (m) {
        case Milestone::ClientInitialized: return true;
        case Milestone::SampLoaded:        return h(Milestone::ClientInitialized);
        case Milestone::SampReady:         return h(Milestone::SampLoaded);
        case Milestone::GameplayConfirmed: return h(Milestone::SampReady);
        case Milestone::VisualReady:       return h(Milestone::SampReady);
        case Milestone::SessionReady:      return h(Milestone::VisualReady) || h(Milestone::GameplayConfirmed);
        default:                           return false;
    }
}

class MilestoneTracker {
public:
    enum class Result : uint8_t { Recorded, RecordedOutOfOrder, Duplicate, StaleSession, Invalid };

    void Reset(uint64_t session, uint64_t startTick) {
        session_ = session;
        start_ = startTick;
        for (int i = 0; i < kMilestoneCount; ++i) { has_[i] = false; at_[i] = 0; }
    }

    Result Observe(Milestone m, uint64_t session, uint64_t tick) {
        const int i = static_cast<int>(m);
        if (i < 0 || i >= kMilestoneCount) return Result::Invalid;
        if (session == 0 || session != session_) return Result::StaleSession;
        if (has_[i]) return Result::Duplicate;
        const bool inOrder = PrerequisitesMet(m, has_);
        has_[i] = true;
        at_[i] = tick;
        return inOrder ? Result::Recorded : Result::RecordedOutOfOrder;
    }

    bool     Has(Milestone m) const { return has_[static_cast<int>(m)]; }
    uint64_t ElapsedMs(Milestone m) const { return Has(m) ? at_[static_cast<int>(m)] - start_ : 0; }
    uint64_t Session() const { return session_; }

    // Healthy gameplay requires the client's gameplay confirmation; visual-ready
    // also proves SA-MP drew a frame after CNetGame. SAMP_READY alone is not enough.
    bool HealthyGameplay() const { return Has(Milestone::GameplayConfirmed); }
    bool StartupConfirmed() const { return Has(Milestone::GameplayConfirmed) || Has(Milestone::VisualReady); }

private:
    uint64_t session_ = 0;
    uint64_t start_ = 0;
    bool     has_[kMilestoneCount] = {};
    uint64_t at_[kMilestoneCount] = {};
};

inline const char* ToString(MilestoneTracker::Result r) {
    switch (r) {
        case MilestoneTracker::Result::Recorded:           return "recorded";
        case MilestoneTracker::Result::RecordedOutOfOrder: return "recorded OUT OF ORDER";
        case MilestoneTracker::Result::Duplicate:          return "duplicate ignored";
        case MilestoneTracker::Result::StaleSession:       return "stale session ignored";
        case MilestoneTracker::Result::Invalid:            return "invalid";
    }
    return "?";
}

} // namespace skr::ipc
