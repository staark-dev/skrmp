#include "skrac/mock_adapter.h"

namespace skr::ac {

bool MockAcAdapter::Start(const char* sessionId) {
    if (!sessionId) return false;
    state_ = AcState::Ready;
    return true;
}

AcStatus MockAcAdapter::Poll() {
    return {state_, "mock-0.0.0", "mock-policy-0"};
}

void MockAcAdapter::Stop() noexcept {
    state_ = AcState::Unknown;
}

} // namespace skr::ac
