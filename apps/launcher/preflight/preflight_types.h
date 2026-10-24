#pragma once
// SKR Enhanced — Preflight Types
// Task #21.2: Pre-launch game integrity check types and data structures.

#include <cstdint>
#include <string>
#include <vector>
#include <array>

namespace skr::preflight {

// ── Check result status ─────────────────────────────────────────────

enum class CheckStatus : uint8_t {
    Pending,         // Not yet evaluated
    Verified,        // Passed all checks
    Warning,         // Non-blocking issue found
    ActionRequired,  // User action needed before launch
    Unsupported,     // Hard block — cannot proceed
};

inline const char* StatusToString(CheckStatus s) {
    switch (s) {
        case CheckStatus::Pending:        return "PENDING";
        case CheckStatus::Verified:       return "VERIFIED";
        case CheckStatus::Warning:        return "WARNING";
        case CheckStatus::ActionRequired: return "ACTION REQUIRED";
        case CheckStatus::Unsupported:    return "UNSUPPORTED";
    }
    return "UNKNOWN";
}

// True for statuses that block launch
inline bool IsBlocking(CheckStatus s) {
    return s == CheckStatus::ActionRequired || s == CheckStatus::Unsupported;
}

// ── Structured check result ─────────────────────────────────────────

struct CheckResult {
    CheckStatus  status     = CheckStatus::Pending;
    std::string  code;          // Machine-readable diagnostic code (e.g. "GTA_NOT_FOUND")
    std::string  summary;       // One-line human-readable summary
    std::string  detail;        // Extended detail for diagnostics / log
    std::string  remediation;   // Suggested fix for the user
};

// ── GTA executable variants ─────────────────────────────────────────

enum class GtaExeVariant : uint8_t {
    Unknown,
    V10USCompact,      // GTA San Andreas 1.0 US compact executable
    V10USHoodlum,      // GTA San Andreas 1.0 US Hoodlum executable
    V10EU,             // 1.0 European
    V11,               // Patch 1.01
    V20,               // Patch 2.0 (Hot Coffee removal)
    Steam,             // Steam version (3.0 / non-1.0)
    WarDrum,           // War Drum Studios (mobile port variant)
};

inline const char* VariantToString(GtaExeVariant v) {
    switch (v) {
        case GtaExeVariant::Unknown:       return "Unknown";
        case GtaExeVariant::V10USCompact:  return "1.0 US (Compact)";
        case GtaExeVariant::V10USHoodlum:  return "1.0 US (Hoodlum)";
        case GtaExeVariant::V10EU:         return "1.0 EU";
        case GtaExeVariant::V11:           return "1.01";
        case GtaExeVariant::V20:           return "2.0";
        case GtaExeVariant::Steam:         return "Steam";
        case GtaExeVariant::WarDrum:       return "War Drum Studios";
    }
    return "Unknown";
}

// ── Executable fingerprint ──────────────────────────────────────────

struct ExeFingerprint {
    GtaExeVariant variant     = GtaExeVariant::Unknown;
    std::array<uint8_t, 32> sha256 = {};  // Full SHA-256
    std::array<uint8_t, 32> sectionLayoutSha256 = {};
    uint16_t peMachine        = 0;
    uint16_t sectionCount     = 0;
    uint32_t peTimestamp      = 0;        // PE TimeDateStamp
    uint32_t peImageSize      = 0;        // PE SizeOfImage
    uint64_t fileSize         = 0;        // File size in bytes
    std::vector<std::string> sectionNames;
    std::string parseError;
    std::string matchedProfileId;
    bool fileReadable         = false;
    bool peValid              = false;
    bool sha256Computed       = false;
    bool structuralMatch      = false;    // Family recognition only; never approval
    bool identified           = false;    // Recognized family, not compatibility approval
    bool exactFingerprintVerified = false;
    bool compatibilityApproved    = false;
};

// ── SA-MP component info ────────────────────────────────────────────

struct SampComponent {
    std::wstring path;
    std::string  name;        // "samp.dll", "samp.exe", etc.
    uint32_t     fileSize = 0;
    std::array<uint8_t, 32> sha256 = {};
    std::string  recognizedVersion; // "0.3.7-R5", "0.3.7-R4", etc. or empty
};

struct SampInspectionResult {
    bool hasSampExe         = false;
    bool hasSampDll         = false;
    bool hasSampCfg         = false;
    bool hasSaMpCfg         = false;
    std::vector<SampComponent> components;
    std::vector<std::wstring>  asiLoaders;  // Potential conflicting ASI files
    std::vector<std::wstring>  conflictDlls; // Other DLLs that may interfere
};

// ── Dependency status ───────────────────────────────────────────────

enum class DepCategory : uint8_t {
    Required,          // Must be present to launch
    Optional,          // Feature degradation if missing
    System,            // Always present on target Windows
    Redistributable,   // Needs separate install (e.g. DirectX, VC++)
};

struct DependencyStatus {
    std::string  name;           // DLL or component name
    DepCategory  category = DepCategory::Required;
    bool         present  = false;
    std::wstring foundPath;      // Where it was found
    std::string  installSource;  // How to get it if missing
    std::string  detail;
};

// ── Full preflight report ───────────────────────────────────────────

struct PreflightReport {
    // Stage 1: GTA Installation
    CheckResult     gtaDiscovery;
    std::wstring    gtaExePath;       // Resolved gta_sa.exe path
    std::wstring    gtaDir;           // Parent directory

    // Stage 2: Executable verification
    CheckResult     exeVerification;
    ExeFingerprint  exeFingerprint;

    // Stage 3: SA-MP components
    CheckResult        sampInspection;
    SampInspectionResult sampResult;

    // Stage 4: Runtime dependencies
    CheckResult     depVerification;
    std::vector<DependencyStatus> dependencies;

    // Stage 5: Bundled runtime
    CheckResult     bundledRuntime;
    std::wstring    bundledSampPath;  // Verified path to bundled samp.dll
    std::wstring    skrClientPath;    // Verified SKR.Client.dll
    std::wstring    cefDir;           // CEF directory

    // Overall
    bool AllMandatoryPassed() const {
        const auto complete = [](CheckStatus status) {
            return status == CheckStatus::Verified || status == CheckStatus::Warning;
        };
        return complete(gtaDiscovery.status) &&
               complete(exeVerification.status) &&
               complete(sampInspection.status) &&
               complete(depVerification.status) &&
               complete(bundledRuntime.status) &&
               exeFingerprint.exactFingerprintVerified &&
               exeFingerprint.compatibilityApproved;
    }
};

// ── Preflight stage enum (for UI progress) ──────────────────────────

enum class PreflightStage : uint8_t {
    LocatingGta = 0,
    VerifyingExe,
    ScanningSamp,
    CheckingDeps,
    PreparingLauncher,
    Complete,
};

inline const wchar_t* StageToLabel(PreflightStage s) {
    switch (s) {
        case PreflightStage::LocatingGta:        return L"Locating GTA San Andreas";
        case PreflightStage::VerifyingExe:        return L"Verifying executable";
        case PreflightStage::ScanningSamp:        return L"Scanning SA-MP components";
        case PreflightStage::CheckingDeps:        return L"Checking runtime dependencies";
        case PreflightStage::PreparingLauncher:   return L"Preparing launcher";
        case PreflightStage::Complete:            return L"Verification complete";
    }
    return L"";
}

constexpr int kPreflightStageCount = 5;

} // namespace skr::preflight
