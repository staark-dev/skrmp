#pragma once

#include <atomic>
#include <cstdint>

namespace skr::bootstrap {

enum class InitializationState : uint32_t {
    NOT_STARTED = 0,
    INITIALIZING = 1,
    WAITING_FOR_D3D9 = 2,
    WAITING_FOR_SAMP = 3,
    HOOKS_INSTALLED = 4,
    FAILED = 5,
};

enum class ComponentState : uint8_t {
    NOT_STARTED = 0,
    WAITING,
    INSTALLED,
    FAILED,
};

enum class InitializationError : uint32_t {
    NONE = 0,
    REQUEST_REJECTED,
    LAUNCH_PARAMS_INVALID,
    SAMP_PRELOAD_FAILED,
    D3D9_THREAD_START_FAILED,
    SAMP_THREAD_START_FAILED,
    MONITOR_THREAD_START_FAILED,
    AC_INITIALIZATION_FAILED,
    D3D9_HOOK_FAILED,
    SAMP_HOOK_FAILED,
    INITIALIZATION_TIMEOUT,
};

class InitializationTracker {
public:
    void Reset() noexcept;
    void Begin() noexcept;
    void SetD3D9(ComponentState state) noexcept;
    void SetSamp(ComponentState state) noexcept;
    void Fail(InitializationError error) noexcept;

    InitializationState GetState() const noexcept;
    InitializationError GetError() const noexcept;

private:
    std::atomic<bool> begun_{false};
    std::atomic<ComponentState> d3d9_{ComponentState::NOT_STARTED};
    std::atomic<ComponentState> samp_{ComponentState::NOT_STARTED};
    std::atomic<InitializationError> error_{InitializationError::NONE};
};

} // namespace skr::bootstrap
