#include "apps/launcher/preflight/exe_fingerprint.h"
#include "apps/launcher/preflight/game_installation_detector.h"
#include "apps/launcher/preflight/samp_inspector.h"
#include "apps/launcher/preflight/verification_request_guard.h"

#include <windows.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using namespace skr::preflight;

namespace {
int failures = 0;

void Expect(bool condition, const char* name) {
    if (!condition) { std::cerr << "FAIL: " << name << '\n'; ++failures; }
    else std::cout << "PASS: " << name << '\n';
}

void SetHash(const char* text, std::array<uint8_t, 32>& out) {
    for (size_t i = 0; i < out.size(); ++i) {
        unsigned int byte = 0;
        sscanf(text + i * 2, "%02x", &byte);
        out[i] = static_cast<uint8_t>(byte);
    }
}

ExeFingerprint Base() {
    ExeFingerprint fp;
    fp.fileReadable = true;
    fp.peValid = true;
    fp.sha256Computed = true;
    fp.peMachine = IMAGE_FILE_MACHINE_I386;
    return fp;
}

std::wstring TempPath(const wchar_t* name) {
    wchar_t directory[MAX_PATH]{};
    GetTempPathW(MAX_PATH, directory);
    return std::wstring(directory) + name;
}

void WriteSmallFile(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    const char bytes[] = "not a PE";
    DWORD written = 0;
    WriteFile(file, bytes, sizeof(bytes), &written, nullptr);
    CloseHandle(file);
}
} // namespace

int main() {
    {
        auto fp = Base();
        fp.fileSize = 5189632;
        fp.peTimestamp = 0x427101CA;
        fp.peImageSize = 0x008B1000;
        fp.sectionCount = 7;
        fp.sectionNames = {".text", "_rwcseg", ".rdata", ".data", "_TEXT_HA", "_rwdseg", ".rsrc"};
        SetHash("EEA3D3469E5AFB6F3A91556E26FE52E99C52278B3A9F4D5219BC149A39BE8A49", fp.sectionLayoutSha256);
        SetHash("590B4BE1BB005F4E07EDF6B32DF4A95B7BA403FB85472D68AA4275875B345B6F", fp.sha256);
        ClassifyFingerprintV2(fp);
        GtaExeVariant variant{};
        const auto verdict = EvaluateExeFingerprint(fp, variant);
        Expect(variant == GtaExeVariant::V10USCompact, "canonical Compact is a distinct family");
        Expect(fp.exactFingerprintVerified, "canonical Compact full-file hash is recognized");
        Expect(!fp.compatibilityApproved && verdict.code == "EXE_PROFILE_NOT_CERTIFIED",
            "Compact reference cannot approve before SKR hook certification");
    }
    {
        auto fp = Base();
        fp.fileSize = 5190656;
        fp.peTimestamp = 0x427101CA;
        fp.peImageSize = 0x008B1000;
        fp.sectionCount = 7;
        fp.sectionNames = {".text", "_rwcseg", ".rdata", ".data", "_TEXT_HA", "_rwdseg", ".rsrc"};
        SetHash("3F164976FE35A1FDBF942E5E23E56022F615E04EA45F69545E6EE82B65AF9878", fp.sectionLayoutSha256);
        SetHash("8EBC7FF3DCCB209ADB331332E498D23DE9C78FF70382F4012B1C8FD60307AB27", fp.sha256);
        ClassifyFingerprintV2(fp);
        GtaExeVariant variant{};
        Expect(fp.variant == GtaExeVariant::V10USCompact && fp.structuralMatch &&
            EvaluateExeFingerprint(fp, variant).code == "EXE_FAMILY_UNVERIFIED",
            "1,024-byte-larger Compact candidate is recognized but remains unverified");
    }
    {
        auto fp = Base();
        fp.fileSize = 14383616;
        fp.peTimestamp = 0x427101CA;
        fp.peImageSize = 0x01177000;
        fp.sectionCount = 11;
        fp.sectionNames = {".text", ".rdata", ".data", ".idata", ".rsrc", ".reloc", ".HOODLUM"};
        SetHash("3FE96D15AA882270D46FDF945CC77FBF49788921762E746770DEEB828683117E", fp.sectionLayoutSha256);
        SetHash("A559AA772FD136379155EFA71F00C47AAD34BBFEAE6196B0FE1047D0645CBD26", fp.sha256);
        ClassifyFingerprintV2(fp);
        GtaExeVariant variant{};
        const auto verdict = EvaluateExeFingerprint(fp, variant);
        Expect(variant == GtaExeVariant::V10USHoodlum && fp.structuralMatch,
            "canonical Hoodlum structure is recognized independently");
        Expect(fp.exactFingerprintVerified && !fp.compatibilityApproved &&
            verdict.code == "EXE_PROFILE_NOT_CERTIFIED",
            "canonical Hoodlum identity remains blocked pending SKR hook certification");
    }
    {
        auto fp = Base();
        fp.fileSize = 14383616;
        fp.peTimestamp = 0x427101CA;
        fp.peImageSize = 0x01177000;
        fp.sectionCount = 11;
        fp.sectionNames = {".text", ".HOODLUM"};
        SetHash("3FE96D15AA882270D46FDF945CC77FBF49788921762E746770DEEB828683117E", fp.sectionLayoutSha256);
        SetHash("0000000000000000000000000000000000000000000000000000000000000001", fp.sha256);
        ClassifyFingerprintV2(fp);
        GtaExeVariant variant{};
        Expect(EvaluateExeFingerprint(fp, variant).code == "EXE_FAMILY_UNVERIFIED",
            "modified Hoodlum-like executable cannot inherit canonical approval");
    }
    {
        auto fp = Base();
        fp.fileSize = 14383616;
        fp.peTimestamp = 0x3EBF1CB0;
        fp.peImageSize = 0x00E3C000;
        ClassifyFingerprintV2(fp);
        Expect(fp.variant == GtaExeVariant::V10EU && !fp.exactFingerprintVerified,
            "recognized unsupported EU metadata remains informational");
    }
    {
        auto fp = Base();
        fp.fileSize = 1234567;
        fp.peTimestamp = 1;
        fp.peImageSize = 2;
        ClassifyFingerprintV2(fp);
        Expect(!fp.identified, "unknown valid PE remains unknown");
    }
    {
        auto fp = Base();
        fp.fileSize = 5189632;
        fp.peTimestamp = 0x427101CA;
        fp.peImageSize = 0x008B1000;
        fp.sectionCount = 7;
        fp.sectionNames = {".text", "_rwcseg", ".rdata", ".data", "_TEXT_HA", "_rwdseg", ".rsrc"};
        SetHash("EEA3D3469E5AFB6F3A91556E26FE52E99C52278B3A9F4D5219BC149A39BE8A49", fp.sectionLayoutSha256);
        SetHash("590B4BE1BB005F4E07EDF6B32DF4A95B7BA403FB85472D68AA4275875B345B60", fp.sha256);
        ClassifyFingerprintV2(fp);
        Expect(fp.identified && !fp.exactFingerprintVerified,
            "modified Compact with unchanged metadata fails exact identity");
    }
    {
        auto fp = Base();
        fp.fileSize = 14383616;
        fp.peTimestamp = 0x427101CA;
        fp.peImageSize = 0x01177000;
        fp.sectionCount = 11;
        fp.sectionNames = {".text", ".HOODLUM"};
        SetHash("3FE96D15AA882270D46FDF945CC77FBF49788921762E746770DEEB828683117E", fp.sectionLayoutSha256);
        SetHash("A5112E9CBBB0D06F271399697ABE2A4E76AA8EA94568F51DFEA19117518D7521", fp.sha256);
        ClassifyFingerprintV2(fp);
        GtaExeVariant variant{};
        Expect(EvaluateExeFingerprint(fp, variant).code == "EXE_BLOCKED_HASH",
            "flagged Hoodlum-like hash is explicitly blocked");
    }

    const auto missing = FingerprintExecutable(TempPath(L"skr-preflight-missing.exe"));
    Expect(!missing.fileReadable, "missing executable is rejected");

    const auto corruptPath = TempPath(L"skr-preflight-corrupt.exe");
    WriteSmallFile(corruptPath);
    const auto corrupt = FingerprintExecutable(corruptPath);
    Expect(corrupt.fileReadable && !corrupt.peValid, "corrupted PE headers are rejected");

    const auto lockedPath = TempPath(L"skr-preflight-locked.exe");
    WriteSmallFile(lockedPath);
    HANDLE lock = CreateFileW(lockedPath.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    const auto unreadable = FingerprintExecutable(lockedPath);
    Expect(!unreadable.fileReadable, "sharing-denied executable is rejected as unreadable");
    CloseHandle(lock);

    const auto original = TempPath(L"skr-preflight-original.exe");
    const auto alias = TempPath(L"skr-preflight-alias.exe");
    WriteSmallFile(original);
    DeleteFileW(alias.c_str());
    const BOOL linked = CreateHardLinkW(alias.c_str(), original.c_str(), nullptr);
    std::vector<GtaCandidate> candidates = {
        {original, L"C:\\one", "test"}, {alias, L"C:\\two", "test"}};
    DeduplicateGtaCandidates(candidates);
    Expect(linked && candidates.size() == 1 && candidates[0].aliases.size() == 2,
        "hard-linked installations collapse while preserving aliases");

    Expect(IsCurrentVerification(7, 7, L"C:\\game\\gta_sa.exe", L"c:\\GAME\\gta_sa.exe"),
        "matching verification request is current");
    Expect(!IsCurrentVerification(8, 7, L"C:\\new\\gta_sa.exe", L"C:\\old\\gta_sa.exe"),
        "stale asynchronous result is rejected");

    {
        SampInspectionResult inspection;
        SampComponent bundled;
        bundled.name = "bundled:samp.dll";
        inspection.components.push_back(bundled);
        Expect(EvaluateSampInspection(inspection, L"").code == "SAMP_BUNDLED_HASH_MISMATCH",
            "unrecognized bundled samp.dll is blocking");
        inspection.components[0].recognizedVersion = "0.3.7-R5";
        Expect(EvaluateSampInspection(inspection, L"").status == CheckStatus::Verified,
            "pinned R5 bundled runtime passes identity validation");
    }
    {
        PreflightReport report;
        report.gtaDiscovery.status = CheckStatus::Verified;
        report.exeVerification.status = CheckStatus::Verified;
        report.sampInspection.status = CheckStatus::Warning;
        report.depVerification.status = CheckStatus::Verified;
        report.bundledRuntime.status = CheckStatus::Verified;
        report.exeFingerprint.exactFingerprintVerified = true;
        report.exeFingerprint.compatibilityApproved = true;
        Expect(report.AllMandatoryPassed(), "ASI-style warning remains nonblocking after all native gates pass");
        report.depVerification.status = CheckStatus::Pending;
        Expect(!report.AllMandatoryPassed(), "pending mandatory result remains blocking");
        report.depVerification.status = CheckStatus::Verified;
        report.bundledRuntime.status = CheckStatus::ActionRequired;
        Expect(!report.AllMandatoryPassed(), "missing runtime remains blocking");
    }

    DeleteFileW(alias.c_str());
    DeleteFileW(original.c_str());
    DeleteFileW(lockedPath.c_str());
    DeleteFileW(corruptPath.c_str());
    return failures == 0 ? 0 : 1;
}
