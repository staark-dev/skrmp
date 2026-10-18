#pragma once
// Small, byte-verified fixes for GTA SA 1.0 US code. Each one patches only
// when the original instructions are found exactly; otherwise it is skipped.

namespace skr::gta_fixes {

// Call before GTA's main thread runs (SkrInitialize). Returns how many fixes applied.
int InstallGtaFixes();

} // namespace skr::gta_fixes
