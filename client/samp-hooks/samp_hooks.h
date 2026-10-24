#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "bootstrap/init_state.h"

namespace skr::samp_hooks {

// Start a background thread that waits for samp.dll to initialise,
// then installs all SKR overrides (chat filter, splash skip, auto-spawn).
bool InstallSampHooks();
skr::bootstrap::ComponentState GetSampHookState() noexcept;

void RemoveSampHooks();

} // namespace skr::samp_hooks
