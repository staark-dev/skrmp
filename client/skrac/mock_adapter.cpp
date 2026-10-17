#include "skrac/mock_adapter.h"
#include <cstring>
#include <chrono>

namespace skr::ac {

static uint64_t NowMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

MockAcAdapter::MockAcAdapter(MockScenario scenario)
    : scenario_(scenario) {}

bool MockAcAdapter::Start(const char* sessionId) {
    if (!sessionId || sessionId[0] == '\0') return false;
    pollCount_ = 0;
    heartbeatSeq_ = 0;
    startTimeMs_ = NowMs();

    switch (scenario_) {
        case MockScenario::DelayedStart:
            state_ = AcState::Starting;
            break;
        case MockScenario::FailAfterN:
        case MockScenario::AlwaysReady:
        case MockScenario::IntermittentDegraded:
        case MockScenario::TimeoutOnPoll:
            state_ = AcState::Ready;
            break;
    }
    return true;
}

AcState MockAcAdapter::GetState() {
    return state_;
}

bool MockAcAdapter::PollStatus(AcStatusResult& out) {
    ++pollCount_;
    memset(&out, 0, sizeof(out));
    out.structSize = sizeof(AcStatusResult);
    out.uptimeMs = NowMs() - startTimeMs_;
    out.heartbeatSeq = heartbeatSeq_;

    switch (scenario_) {
        case MockScenario::AlwaysReady:
            out.state = AcState::Ready;
            break;

        case MockScenario::DelayedStart:
            if (out.uptimeMs < 2000) {
                out.state = AcState::Starting;
            } else {
                state_ = AcState::Ready;
                out.state = AcState::Ready;
            }
            break;

        case MockScenario::IntermittentDegraded:
            out.state = (pollCount_ % 5 == 0) ? AcState::Degraded : AcState::Ready;
            state_ = out.state;
            break;

        case MockScenario::TimeoutOnPoll:
            return false;

        case MockScenario::FailAfterN:
            if (pollCount_ > 10) {
                state_ = AcState::Failed;
                out.state = AcState::Failed;
            } else {
                out.state = AcState::Ready;
            }
            break;
    }

    strncpy(out.engineVersion, "mock-0.0.0", kMaxVersionLen - 1);
    strncpy(out.policyVersion, "mock-policy-0", kMaxVersionLen - 1);
    return true;
}

bool MockAcAdapter::GetHeartbeat(AcHeartbeat& out) {
    if (state_ == AcState::Unknown) return false;

    memset(&out, 0, sizeof(out));
    out.structSize = sizeof(AcHeartbeat);
    out.sequence = ++heartbeatSeq_;
    out.timestampMs = NowMs();
    out.state = state_;
    out.lastVerdict = AcVerdict::Clean;
    return true;
}

void MockAcAdapter::Stop() noexcept {
    state_ = AcState::Unknown;
    pollCount_ = 0;
    heartbeatSeq_ = 0;
}

} // namespace skr::ac
