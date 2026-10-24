#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>
#include "preflight/preflight_types.h"

namespace skr::launcher {

struct PreflightWindowResult {
    bool approved = false;
    bool webViewAvailable = true;
    skr::preflight::PreflightReport report;
};

// Hosts the production preflight UI in a compact WebView2 window. The native
// PreflightReport remains the sole source of compatibility approval.
PreflightWindowResult ShowPreflightExperience(HINSTANCE instance,
                                               const std::wstring& preflightDir,
                                               const std::wstring& launcherDir,
                                               const std::wstring& preferredGtaPath = {});

} // namespace skr::launcher
