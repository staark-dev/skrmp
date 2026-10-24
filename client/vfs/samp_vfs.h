#pragma once
// ─────────────────────────────────────────────────────────────────────
// SKR Virtual Filesystem — TEST_ONLY SA-MP Resource Redirection
// ─────────────────────────────────────────────────────────────────────
// Hooks Windows file APIs (CreateFileW/A, GetFileAttributesW/A,
// FindFirstFileExW) to trace resource-file requests during SA-MP R5
// initialization and selectively redirect verified SA-MP resource
// paths from the GTA installation directory to the bundled
// Release\samp\ directory.
//
// Constraints:
//   - Preserves GTA's real executable path and process working directory
//   - Preserves -c bootstrap behavior (command-line swap is separate)
//   - Only redirects known SA-MP resource filenames; all other I/O passes through
//   - Does not modify the GTA installation directory
//   - Does not change existing game hooks
//   - Gated behind #ifdef SKR_TEST_CERTIFICATION
//
// Install BEFORE LoadLibraryExW(samp.dll) so the hooks are active
// during samp.dll's DllMain (which runs ParseCommandLine and may
// probe for resource files).
// ─────────────────────────────────────────────────────────────────────

#ifndef SKR_TEST_CERTIFICATION
#error "samp_vfs.h must only be included in TEST_ONLY builds (SKR_TEST_CERTIFICATION=1)"
#endif

#include <string>

namespace skr::vfs {

struct VfsConfig {
    std::wstring sampResourceDir;   // Bundled SA-MP runtime directory (trailing backslash)
    std::wstring gtaDir;            // GTA installation directory (no trailing backslash)
    bool traceAll = true;           // Log ALL file operations under GTA dir (verbose)
};

// Install VFS inline hooks on kernel32 file APIs.
// Returns true if all hooks installed successfully.
// Non-fatal on failure: logs the reason and samp.dll loading proceeds
// without redirection (which may or may not cause error 1114).
bool Install(const VfsConfig& config);

// Remove all VFS hooks, restoring original kernel32 function bytes.
// Safe to call even if Install() was never called or failed.
void Remove();

// Returns true if VFS hooks are currently active.
bool IsInstalled();

// Returns a summary of traced file operations for diagnostics.
// Call after samp.dll DllMain has executed (success or failure).
void LogTraceSummary();

} // namespace skr::vfs
