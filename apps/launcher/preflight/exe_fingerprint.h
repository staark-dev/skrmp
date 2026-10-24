#pragma once
// SKR Enhanced — Executable Fingerprint Database
// Task #21.2: Identifies GTA SA executable variants via PE metadata + SHA-256.
// Supports: v1.0 US, v1.0 US Hoodlum, v1.0 EU, v1.1, v2.0, Steam.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>
#include "preflight_types.h"

namespace skr::preflight {

// Read PE COFF + Optional headers from an executable file.
// Returns false if the file cannot be opened or parsed.
struct PeHeaders {
    DWORD machine;       // IMAGE_FILE_HEADER.Machine (0x14C = x86)
    DWORD timestamp;     // IMAGE_FILE_HEADER.TimeDateStamp
    DWORD imageSize;     // IMAGE_OPTIONAL_HEADER.SizeOfImage
    WORD  subsystem;     // IMAGE_OPTIONAL_HEADER.Subsystem
    WORD  sectionCount;
    bool  is32bit;       // true if PE32 (not PE32+)
    std::vector<IMAGE_SECTION_HEADER> sections;
};
bool ReadPeHeaders(const std::wstring& exePath, PeHeaders& out);

// Compute SHA-256 hash of a file using BCrypt.
// Returns empty string on failure.
std::string ComputeFileSha256(const std::wstring& filePath);

// Convert a SHA-256 byte array to lowercase hex string.
std::string Sha256ToString(const unsigned char hash[32]);

// Full fingerprinting: reads PE headers, computes SHA-256, identifies variant.
ExeFingerprint FingerprintExecutable(const std::wstring& exePath);

// Applies the production V2 identity database to an already populated
// fingerprint. Exposed so table-driven tests can exercise classification
// without shipping copyrighted executable fixtures.
void ClassifyFingerprintV2(ExeFingerprint& fingerprint);

// Evaluate a fingerprint and produce a CheckResult.
// Sets outVariant to the identified variant for downstream use.
CheckResult EvaluateExeFingerprint(const ExeFingerprint& fp,
                                    GtaExeVariant& outVariant);

} // namespace skr::preflight
