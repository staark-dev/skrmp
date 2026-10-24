#pragma once
// SKR Enhanced — SA-MP Installation Inspector
// Task #21.2: Scans GTA directory for SA-MP components, ASI loaders,
// and potential conflict DLLs. Validates bundled samp.dll hash.

#include <string>
#include "preflight_types.h"

namespace skr::preflight {

// Canonical SA-MP 0.3.7-R5 SHA-256 hash (matches samp_loader.cpp)
extern const char kR5Sha256[];

// Inspects the GTA SA directory for SA-MP components and potential conflicts.
SampInspectionResult InspectSampInstallation(const std::wstring& gtaDir,
                                              const std::wstring& launcherDir);

// Evaluates the SA-MP inspection and produces a summary CheckResult.
CheckResult EvaluateSampInspection(const SampInspectionResult& inspection,
                                    const std::wstring& bundledSampPath);

} // namespace skr::preflight
