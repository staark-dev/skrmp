#pragma once

#include <cstdint>

namespace skr::native_pause::controls::logic {

enum class CapturePhase : uint8_t { Idle, WaitingForRelease, Armed };
enum class CaptureEvent : uint8_t { Other, Release, Escape, FocusLost, SupportedInput };

struct CaptureMachine {
    CapturePhase phase = CapturePhase::Idle;

    void Begin() { phase = CapturePhase::WaitingForRelease; }
    bool Active() const { return phase != CapturePhase::Idle; }
    bool Armed() const { return phase == CapturePhase::Armed; }
};

enum class CaptureResult : uint8_t { Ignored, Consumed, Cancelled, Captured };
CaptureResult AdvanceCapture(CaptureMachine& machine, CaptureEvent event);

enum class ActionContext : uint8_t {
    FirstPerson,
    ThirdPerson,
    InCar,
    InCarThirdPerson,
    Common,
    FirstAndThirdPerson,
    None,
};

bool ContextsConflict(ActionContext lhs, ActionContext rhs);
bool ReadbackMatches(uint32_t expected, uint32_t actual, bool saveSucceeded);

enum class TransactionDecision : uint8_t { Commit, Rollback };
TransactionDecision EvaluateTransaction(bool mutationSucceeded, bool saveSucceeded,
                                        bool readbackSucceeded, bool conflictsCleared);

enum class ConflictDecision : uint8_t { Cancel, Replace };
ConflictDecision ChooseConflictDecision(bool replace);

} // namespace skr::native_pause::controls::logic
