#pragma once
// SKR Enhanced — Preflight Controller
// Task #21.2: Orchestrates all preflight checks and drives the native UI.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>
#include <functional>
#include "preflight_types.h"

namespace skr::preflight {

// Callback for UI progress updates.
using PreflightProgressCallback = std::function<void(PreflightStage stage,
                                                      const CheckResult& result)>;

// Runs the complete preflight sequence. Blocks until all checks complete.
PreflightReport RunPreflight(const std::wstring& launcherDir,
                              PreflightProgressCallback onProgress = {});

// Writes a diagnostic log to %APPDATA%\SKR\preflight.log.
bool WriteDiagnosticLog(const PreflightReport& report,
                         const std::wstring& gtaDir,
                         const std::wstring& launcherDir);

// Shows the native Win32 preflight window. Blocks until user acknowledges.
// Returns true if user chose to continue (all mandatory checks passed).
bool ShowPreflightWindow(HINSTANCE hInstance, const PreflightReport& report);

} // namespace skr::preflight
