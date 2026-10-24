#include "samp_inspector.h"
#include "exe_fingerprint.h" // ComputeFileSha256, Sha256ToString
#include <shlobj.h>
#include <algorithm>

namespace skr::preflight {

// Canonical R5 hash — mirrors samp_loader.cpp line 41-43
const char kR5Sha256[] = "B72B5DBE725F81864CA3F78BC7063BDA56CC05FC7188AF822FA7A754432553A2";

// ── Helpers ─────────────────────────────────────────────────────────

static bool FileExists(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static uint32_t GetFileSize32(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA attrs{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attrs))
        return 0;
    return attrs.nFileSizeLow;
}

static std::wstring ToLower(std::wstring s) {
    for (auto& c : s) c = towlower(c);
    return s;
}

// Identify SA-MP version from SHA-256 hex string
static std::string IdentifySampVersion(const std::string& sha256Hex) {
    // Case-insensitive compare
    std::string upper = sha256Hex;
    for (auto& c : upper) c = (char)toupper((unsigned char)c);
    if (upper == kR5Sha256) return "0.3.7-R5";
    return {};
}

// Check if a filename looks like an ASI loader or conflicting component
static bool IsAsiLoader(const std::wstring& filename) {
    std::wstring lower = ToLower(filename);
    return lower == L"asi.dll" ||
           lower == L"d3d9.dll" ||
           lower == L"dinput8.dll" ||
           lower == L"vorbisfile.dll" ||
           lower == L"cleo.asi" ||
           lower == L"sampfuncs.asi" ||
           lower == L"sobeit.asi" ||
           lower == L"modloader.asi";
}

static bool IsConflictDll(const std::wstring& filename) {
    std::wstring lower = ToLower(filename);
    return lower == L"bass.dll" ||
           lower == L"d3d9.dll" ||
           lower == L"dxgi.dll" ||
           lower == L"dsound.dll" ||
           lower == L"eax.dll";
}

// ── Scan implementation ─────────────────────────────────────────────

SampInspectionResult InspectSampInstallation(const std::wstring& gtaDir,
                                              const std::wstring& launcherDir) {
    SampInspectionResult result;

    auto scanComponent = [&](const std::wstring& path, const std::string& name) {
        if (!FileExists(path)) return;

        SampComponent comp;
        comp.path = path;
        comp.name = name;
        comp.fileSize = GetFileSize32(path);

        // Compute SHA-256 and parse back to byte array
        std::string hashStr = ComputeFileSha256(path);
        if (hashStr.size() == 64) {
            for (int i = 0; i < 32; ++i) {
                unsigned int byte;
                sscanf(hashStr.c_str() + i * 2, "%02x", &byte);
                comp.sha256[i] = (uint8_t)byte;
            }
        }

        if (name == "samp.dll" || name == "bundled:samp.dll") {
            if (name == "samp.dll") result.hasSampDll = true;
            comp.recognizedVersion = IdentifySampVersion(hashStr);
        }

        result.components.push_back(comp);
    };

    // Core SA-MP components in GTA dir
    scanComponent(gtaDir + L"\\samp.dll", "samp.dll");
    scanComponent(gtaDir + L"\\samp.exe", "samp.exe");
    scanComponent(gtaDir + L"\\samp.dll.skr", "samp.dll.skr");
    scanComponent(gtaDir + L"\\d3d9.dll.skr", "d3d9.dll.skr");

    result.hasSampExe = FileExists(gtaDir + L"\\samp.exe");
    result.hasSampCfg = FileExists(gtaDir + L"\\sa-mp.cfg");

    // SAMP folder in user documents
    wchar_t docPath[MAX_PATH]{};
    if (SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, 0, docPath) == S_OK) {
        std::wstring sampCfg = std::wstring(docPath) +
                               L"\\GTA San Andreas User Files\\SAMP\\sa-mp.cfg";
        result.hasSaMpCfg = FileExists(sampCfg);
    }

    // Bundled samp directory
    std::wstring bundledSamp = launcherDir + L"\\samp\\samp.dll";
    scanComponent(bundledSamp, "bundled:samp.dll");

    // Scan for ASI loaders and conflict DLLs in GTA dir
    WIN32_FIND_DATAW findData;
    HANDLE hFind = FindFirstFileW((gtaDir + L"\\*").c_str(), &findData);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

            std::wstring name(findData.cFileName);
            std::wstring lower = ToLower(name);

            if (lower.size() > 4 && lower.substr(lower.size() - 4) == L".asi") {
                result.asiLoaders.push_back(name);
            }

            if (IsAsiLoader(name)) {
                if (lower.size() <= 4 || lower.substr(lower.size() - 4) != L".asi") {
                    result.asiLoaders.push_back(name);
                }
            }

            if (IsConflictDll(name)) {
                result.conflictDlls.push_back(name);
            }
        } while (FindNextFileW(hFind, &findData));
        FindClose(hFind);
    }

    return result;
}

CheckResult EvaluateSampInspection(const SampInspectionResult& inspection,
                                    const std::wstring& /*bundledSampPath*/) {
    CheckResult result;

    bool hasLegacySamp = false;
    bool legacyIsR5 = false;
    bool hasBundledSamp = false;
    bool bundledIsR5 = false;
    std::string legacyVersion;

    for (auto& comp : inspection.components) {
        if (comp.name == "samp.dll") {
            hasLegacySamp = true;
            legacyVersion = comp.recognizedVersion;
            legacyIsR5 = (comp.recognizedVersion == "0.3.7-R5");
        }
        if (comp.name == "bundled:samp.dll") {
            hasBundledSamp = true;
            bundledIsR5 = (comp.recognizedVersion == "0.3.7-R5");
        }
    }

    if (hasBundledSamp && !bundledIsR5) {
        result.status = CheckStatus::Unsupported;
        result.code = "SAMP_BUNDLED_HASH_MISMATCH";
        result.summary = "Bundled SA-MP runtime failed identity verification";
        result.detail = "samp\\samp.dll is present, but its SHA-256 is not the pinned 0.3.7-R5 identity";
        result.remediation = "Restore the authorized pinned R5 runtime package. Do not use an arbitrary samp.dll.";
    } else if (!inspection.asiLoaders.empty()) {
        result.status = CheckStatus::Warning;
        result.code = "SAMP_ASI_PRESENT";
        result.summary = "ASI loader(s) detected in GTA directory";
        result.detail = "Found " + std::to_string(inspection.asiLoaders.size()) +
                        " ASI file(s) that may conflict with SKR injection";
        result.remediation = "Consider removing third-party ASI mods from the "
                             "GTA directory for best compatibility.";
    } else if (hasLegacySamp && !legacyIsR5) {
        result.status = CheckStatus::Warning;
        result.code = "SAMP_VERSION_MISMATCH";
        result.summary = "Existing SA-MP in GTA directory is not 0.3.7-R5";
        result.detail = "Found samp.dll" +
                        (legacyVersion.empty() ? "" : " (version " + legacyVersion + ")") +
                        " — SKR will use its bundled copy instead";
        result.remediation = "No action required. SKR uses its own bundled SA-MP runtime.";
    } else if (!inspection.conflictDlls.empty()) {
        result.status = CheckStatus::Warning;
        result.code = "SAMP_CONFLICT_DLLS";
        result.summary = "Potentially conflicting DLLs found in GTA directory";
        result.detail = std::to_string(inspection.conflictDlls.size()) +
                        " DLL(s) may interfere with SA-MP";
        result.remediation = "If you experience issues, try removing custom "
                             "d3d9.dll or other wrapper DLLs from the GTA folder.";
    } else {
        result.status = CheckStatus::Verified;
        result.code = "SAMP_CLEAN";
        result.summary = "SA-MP environment is clean";
        if (hasLegacySamp && legacyIsR5) {
            result.detail = "Existing SA-MP 0.3.7-R5 found — compatible";
        }
    }

    return result;
}

} // namespace skr::preflight
