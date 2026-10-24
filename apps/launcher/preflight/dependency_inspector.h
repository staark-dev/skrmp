#pragma once
// SKR Enhanced — Dependency Inspector
// Task #21.2: Checks for runtime dependencies required by GTA SA + SA-MP + SKR.
// Inspects: DirectX 9 redistributables, Visual C++ runtimes, system DLLs.

#include <string>
#include <vector>
#include "preflight_types.h"

namespace skr::preflight {

// Checks all runtime dependencies needed for launch.
std::vector<DependencyStatus> InspectDependencies(const std::wstring& gtaDir);

// Evaluates the dependency list and produces a summary CheckResult.
CheckResult EvaluateDependencies(const std::vector<DependencyStatus>& deps);

// Checks for the bundled SKR runtime (SKR.Client.dll, samp.dll, CEF).
CheckResult EvaluateBundledRuntime(const std::wstring& launcherDir,
                                    const std::wstring& gtaDir,
                                    std::wstring& outBundledSampPath,
                                    std::wstring& outSkrClientPath,
                                    std::wstring& outCefDir);

} // namespace skr::preflight
