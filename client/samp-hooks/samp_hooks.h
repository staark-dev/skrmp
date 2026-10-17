#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace skr::samp_hooks {

// Start a background thread that waits for samp.dll to initialise,
// then installs all SKR overrides (chat filter, splash skip, auto-spawn).
bool InstallSampHooks();

void RemoveSampHooks();

} // namespace skr::samp_hooks
