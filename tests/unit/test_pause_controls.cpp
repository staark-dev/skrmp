#include "native-pause/pause_controls_logic.h"

#include <cassert>
#include <iostream>

using namespace skr::native_pause::controls::logic;

static void TestCaptureLifecycle() {
    CaptureMachine capture{};
    assert(!capture.Active());
    capture.Begin();
    assert(capture.Active() && !capture.Armed());
    assert(AdvanceCapture(capture, CaptureEvent::SupportedInput) == CaptureResult::Consumed);
    assert(!capture.Armed()); // initiating press cannot bind itself
    assert(AdvanceCapture(capture, CaptureEvent::Release) == CaptureResult::Consumed);
    assert(capture.Armed());
    assert(AdvanceCapture(capture, CaptureEvent::SupportedInput) == CaptureResult::Captured);
    assert(!capture.Active());
}

static void TestCancellationAndFocusLoss() {
    CaptureMachine capture{};
    capture.Begin();
    assert(AdvanceCapture(capture, CaptureEvent::Escape) == CaptureResult::Cancelled);
    assert(!capture.Active());
    capture.Begin();
    assert(AdvanceCapture(capture, CaptureEvent::Release) == CaptureResult::Consumed);
    assert(AdvanceCapture(capture, CaptureEvent::FocusLost) == CaptureResult::Cancelled);
    assert(!capture.Active());
}

static void TestConflictContexts() {
    assert(ContextsConflict(ActionContext::ThirdPerson, ActionContext::FirstAndThirdPerson));
    assert(ContextsConflict(ActionContext::InCar, ActionContext::InCarThirdPerson));
    assert(ContextsConflict(ActionContext::Common, ActionContext::InCar));
    assert(!ContextsConflict(ActionContext::FirstPerson, ActionContext::InCar));
    assert(!ContextsConflict(ActionContext::ThirdPerson, ActionContext::InCar));
    assert(!ContextsConflict(ActionContext::None, ActionContext::Common));
}

static void TestTransactionVerificationAndRollbackDecision() {
    assert(ReadbackMatches(42, 42, true));
    assert(!ReadbackMatches(42, 41, true));
    assert(!ReadbackMatches(42, 42, false));
    assert(EvaluateTransaction(true, true, true, true) == TransactionDecision::Commit);
    assert(EvaluateTransaction(false, true, true, true) == TransactionDecision::Rollback);
    assert(EvaluateTransaction(true, false, true, true) == TransactionDecision::Rollback);
    assert(EvaluateTransaction(true, true, false, true) == TransactionDecision::Rollback);
    assert(EvaluateTransaction(true, true, true, false) == TransactionDecision::Rollback);
    assert(ChooseConflictDecision(false) == ConflictDecision::Cancel);
    assert(ChooseConflictDecision(true) == ConflictDecision::Replace);
}

int main() {
    TestCaptureLifecycle();
    TestCancellationAndFocusLoss();
    TestConflictContexts();
    TestTransactionVerificationAndRollbackDecision();
    std::cout << "pause controls logic tests passed\n";
    return 0;
}
