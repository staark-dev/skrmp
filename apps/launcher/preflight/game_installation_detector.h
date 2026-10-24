#pragma once
// SKR Enhanced — Game Installation Detector
// Task #21.2: Locates GTA San Andreas installations via registry, Steam,
// well-known paths, and saved configuration. Does NOT scan entire drives.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>
#include <vector>
#include "preflight_types.h"

namespace skr::preflight {

// Candidate installation found by the detector.
struct GtaCandidate {
    std::wstring exePath;     // Full path to gta_sa.exe
    std::wstring directory;   // Parent directory
    std::string  source;      // How it was found (e.g. "registry", "steam", "config")
    std::vector<std::wstring> aliases; // Other paths to the same physical file
};

// Collapses duplicate paths and NTFS hard links by stable volume/file identity.
// The first candidate remains canonical; every other path is retained as an alias.
void DeduplicateGtaCandidates(std::vector<GtaCandidate>& candidates);

// Searches for GTA SA installations in a deterministic, non-invasive way.
std::vector<GtaCandidate> DetectGtaInstallations(const std::wstring& launcherDir);

// Resolves a single candidate path — accepts either a gta_sa.exe path or a
// directory containing it. Returns empty if the exe does not exist.
std::wstring ResolveGtaExePath(const std::wstring& pathOrDir);

// Builds the preflight CheckResult for GTA discovery.
CheckResult EvaluateGtaDiscovery(const std::vector<GtaCandidate>& candidates,
                                  std::wstring& outExePath,
                                  std::wstring& outDir);

} // namespace skr::preflight
