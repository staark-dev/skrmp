#include "native-pause/pause_controls_logic.h"

namespace skr::native_pause::controls::logic {

CaptureResult AdvanceCapture(CaptureMachine& machine, CaptureEvent event) {
    if (!machine.Active()) return CaptureResult::Ignored;
    if (event == CaptureEvent::Escape || event == CaptureEvent::FocusLost) {
        machine.phase = CapturePhase::Idle;
        return CaptureResult::Cancelled;
    }
    if (machine.phase == CapturePhase::WaitingForRelease) {
        if (event == CaptureEvent::Release) machine.phase = CapturePhase::Armed;
        return CaptureResult::Consumed;
    }
    if (event == CaptureEvent::SupportedInput) {
        machine.phase = CapturePhase::Idle;
        return CaptureResult::Captured;
    }
    return CaptureResult::Consumed;
}

namespace {
constexpr uint8_t kFirst = 1u << 0;
constexpr uint8_t kThird = 1u << 1;
constexpr uint8_t kCar = 1u << 2;

uint8_t ContextMask(ActionContext context) {
    switch (context) {
        case ActionContext::FirstPerson:          return kFirst;
        case ActionContext::ThirdPerson:          return kThird;
        case ActionContext::InCar:                return kCar;
        case ActionContext::InCarThirdPerson:     return kCar | kThird;
        case ActionContext::Common:               return kFirst | kThird | kCar;
        case ActionContext::FirstAndThirdPerson:  return kFirst | kThird;
        case ActionContext::None:                 return 0;
    }
    return 0;
}
} // namespace

bool ContextsConflict(ActionContext lhs, ActionContext rhs) {
    return (ContextMask(lhs) & ContextMask(rhs)) != 0;
}

bool ReadbackMatches(uint32_t expected, uint32_t actual, bool saveSucceeded) {
    return saveSucceeded && expected == actual;
}

TransactionDecision EvaluateTransaction(bool mutationSucceeded, bool saveSucceeded,
                                        bool readbackSucceeded, bool conflictsCleared) {
    return mutationSucceeded && saveSucceeded && readbackSucceeded && conflictsCleared
        ? TransactionDecision::Commit
        : TransactionDecision::Rollback;
}

ConflictDecision ChooseConflictDecision(bool replace) {
    return replace ? ConflictDecision::Replace : ConflictDecision::Cancel;
}

} // namespace skr::native_pause::controls::logic
