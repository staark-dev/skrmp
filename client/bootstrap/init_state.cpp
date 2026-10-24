#include "bootstrap/init_state.h"

namespace skr::bootstrap {

void InitializationTracker::Reset() noexcept {
    error_.store(InitializationError::NONE, std::memory_order_release);
    d3d9_.store(ComponentState::NOT_STARTED, std::memory_order_release);
    samp_.store(ComponentState::NOT_STARTED, std::memory_order_release);
    begun_.store(false, std::memory_order_release);
}

void InitializationTracker::Begin() noexcept {
    error_.store(InitializationError::NONE, std::memory_order_release);
    d3d9_.store(ComponentState::NOT_STARTED, std::memory_order_release);
    samp_.store(ComponentState::NOT_STARTED, std::memory_order_release);
    begun_.store(true, std::memory_order_release);
}

void InitializationTracker::SetD3D9(ComponentState state) noexcept {
    d3d9_.store(state, std::memory_order_release);
}

void InitializationTracker::SetSamp(ComponentState state) noexcept {
    samp_.store(state, std::memory_order_release);
}

void InitializationTracker::Fail(InitializationError error) noexcept {
    error_.store(error, std::memory_order_release);
}

InitializationState InitializationTracker::GetState() const noexcept {
    if (!begun_.load(std::memory_order_acquire)) return InitializationState::NOT_STARTED;
    if (error_.load(std::memory_order_acquire) != InitializationError::NONE)
        return InitializationState::FAILED;

    const auto d3d9 = d3d9_.load(std::memory_order_acquire);
    const auto samp = samp_.load(std::memory_order_acquire);
    if (d3d9 == ComponentState::FAILED || samp == ComponentState::FAILED)
        return InitializationState::FAILED;
    if (d3d9 == ComponentState::INSTALLED && samp == ComponentState::INSTALLED)
        return InitializationState::HOOKS_INSTALLED;
    if (d3d9 == ComponentState::NOT_STARTED && samp == ComponentState::NOT_STARTED)
        return InitializationState::INITIALIZING;
    if (d3d9 == ComponentState::WAITING ||
        (d3d9 == ComponentState::NOT_STARTED && samp != ComponentState::INSTALLED))
        return InitializationState::WAITING_FOR_D3D9;
    if (samp == ComponentState::WAITING || samp == ComponentState::NOT_STARTED)
        return InitializationState::WAITING_FOR_SAMP;
    return InitializationState::INITIALIZING;
}

InitializationError InitializationTracker::GetError() const noexcept {
    return error_.load(std::memory_order_acquire);
}

} // namespace skr::bootstrap
