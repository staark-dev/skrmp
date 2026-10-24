#include "exe_fingerprint.h"

#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace skr::preflight {
namespace {

constexpr char kBlockedLocalSha256[] = "A5112E9CBBB0D06F271399697ABE2A4E76AA8EA94568F51DFEA19117518D7521";
constexpr char kCompactReferenceSha256[] = "590B4BE1BB005F4E07EDF6B32DF4A95B7BA403FB85472D68AA4275875B345B6F";
// Derived locally from the exact sample matching the independently published
// Hoodlum MD5 170b...8a18 and SHA-1 185b...2a7e byte-for-byte.
constexpr char kHoodlumReferenceSha256[] = "A559AA772FD136379155EFA71F00C47AAD34BBFEAE6196B0FE1047D0645CBD26";
constexpr char kHoodlumLayoutSha256[] = "3FE96D15AA882270D46FDF945CC77FBF49788921762E746770DEEB828683117E";
constexpr char kCompactRetailLayoutSha256[] = "3F164976FE35A1FDBF942E5E23E56022F615E04EA45F69545E6EE82B65AF9878";
constexpr char kCompactBaseLayoutSha256[] = "EEA3D3469E5AFB6F3A91556E26FE52E99C52278B3A9F4D5219BC149A39BE8A49";

struct InformationalProfile {
    GtaExeVariant variant;
    uint64_t fileSize;
    uint32_t timestamp;
    uint32_t imageSize;
};

// Legacy metadata is retained for diagnostic naming only. These entries can
// never approve an executable because no provenance-backed full-file hash is known.
constexpr InformationalProfile kInformationalProfiles[] = {
    {GtaExeVariant::V10EU, 14383616, 0x3EBF1CB0, 0x00E3C000},
    {GtaExeVariant::V11,   15806464, 0x40C4B47E, 0x00EB4000},
    {GtaExeVariant::V20,   14999552, 0x4590CEEE, 0x00E58000},
    {GtaExeVariant::Steam,        0, 0x5399A65D, 0x00D02000},
};

bool ReadAt(HANDLE file, uint64_t offset, void* buffer, DWORD size) {
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN)) return false;
    DWORD read = 0;
    return ReadFile(file, buffer, size, &read, nullptr) && read == size;
}

bool ParseHex(const std::string& value, std::array<uint8_t, 32>& out) {
    if (value.size() != 64) return false;
    for (size_t i = 0; i < out.size(); ++i) {
        unsigned int byte = 0;
        if (sscanf(value.c_str() + i * 2, "%02x", &byte) != 1) return false;
        out[i] = static_cast<uint8_t>(byte);
    }
    return true;
}

bool HasSection(const ExeFingerprint& fp, const char* expected) {
    return std::find(fp.sectionNames.begin(), fp.sectionNames.end(), expected) != fp.sectionNames.end();
}

bool HasCompactSections(const ExeFingerprint& fp) {
    static const std::vector<std::string> expected = {
        ".text", "_rwcseg", ".rdata", ".data", "_TEXT_HA", "_rwdseg", ".rsrc"
    };
    return fp.sectionNames == expected;
}

std::string Hex32(uint32_t value) {
    char text[16]{};
    snprintf(text, sizeof(text), "0x%08X", value);
    return text;
}

std::string Hex16(uint16_t value) {
    char text[12]{};
    snprintf(text, sizeof(text), "0x%04X", value);
    return text;
}

bool HashBytes(const uint8_t* data, size_t size, std::array<uint8_t, 32>& out) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0;
    if (ok) ok = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == 0;
    if (ok && size) ok = BCryptHashData(hash, const_cast<PUCHAR>(data), static_cast<ULONG>(size), 0) == 0;
    if (ok) ok = BCryptFinishHash(hash, out.data(), static_cast<ULONG>(out.size()), 0) == 0;
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok;
}

} // namespace

bool ReadPeHeaders(const std::wstring& exePath, PeHeaders& out) {
    out = {};
    HANDLE file = CreateFileW(exePath.c_str(), GENERIC_READ,
        FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER length{};
    IMAGE_DOS_HEADER dos{};
    bool ok = GetFileSizeEx(file, &length) && length.QuadPart >= static_cast<LONGLONG>(sizeof(dos)) &&
        ReadAt(file, 0, &dos, sizeof(dos)) && dos.e_magic == IMAGE_DOS_SIGNATURE &&
        dos.e_lfanew >= static_cast<LONG>(sizeof(dos));
    const uint64_t ntOffset = ok ? static_cast<uint64_t>(dos.e_lfanew) : 0;
    DWORD signature = 0;
    IMAGE_FILE_HEADER coff{};
    if (ok) ok = ntOffset + sizeof(signature) + sizeof(coff) <= static_cast<uint64_t>(length.QuadPart) &&
        ReadAt(file, ntOffset, &signature, sizeof(signature)) && signature == IMAGE_NT_SIGNATURE &&
        ReadAt(file, ntOffset + sizeof(signature), &coff, sizeof(coff));
    if (ok) ok = coff.NumberOfSections > 0 && coff.NumberOfSections <= 96 &&
        coff.SizeOfOptionalHeader >= sizeof(IMAGE_OPTIONAL_HEADER32);

    IMAGE_OPTIONAL_HEADER32 optional{};
    const uint64_t optionalOffset = ntOffset + sizeof(signature) + sizeof(coff);
    if (ok) ok = optionalOffset + coff.SizeOfOptionalHeader <= static_cast<uint64_t>(length.QuadPart) &&
        ReadAt(file, optionalOffset, &optional, sizeof(optional)) &&
        optional.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC;

    const uint64_t sectionOffset = optionalOffset + coff.SizeOfOptionalHeader;
    const uint64_t sectionBytes = static_cast<uint64_t>(coff.NumberOfSections) * sizeof(IMAGE_SECTION_HEADER);
    std::vector<IMAGE_SECTION_HEADER> sections(coff.NumberOfSections);
    if (ok) ok = sectionOffset + sectionBytes <= static_cast<uint64_t>(length.QuadPart) &&
        ReadAt(file, sectionOffset, sections.data(), static_cast<DWORD>(sectionBytes));

    if (ok) {
        out.machine = coff.Machine;
        out.timestamp = coff.TimeDateStamp;
        out.imageSize = optional.SizeOfImage;
        out.subsystem = optional.Subsystem;
        out.sectionCount = coff.NumberOfSections;
        out.is32bit = coff.Machine == IMAGE_FILE_MACHINE_I386;
        out.sections = std::move(sections);
    }
    CloseHandle(file);
    return ok;
}

std::string ComputeFileSha256(const std::wstring& filePath) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return {};
    if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) != 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }
    HANDLE file = CreateFileW(filePath.c_str(), GENERIC_READ,
        FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }
    std::array<uint8_t, 65536> buffer{};
    DWORD read = 0;
    bool ok = true;
    for (;;) {
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) { ok = false; break; }
        if (!read) break;
        if (BCryptHashData(hash, buffer.data(), read, 0) != 0) { ok = false; break; }
    }
    CloseHandle(file);
    std::array<uint8_t, 32> digest{};
    if (ok) ok = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) == 0;
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok ? Sha256ToString(digest.data()) : std::string{};
}

std::string Sha256ToString(const unsigned char hash[32]) {
    char result[65]{};
    for (int i = 0; i < 32; ++i) snprintf(result + i * 2, 3, "%02X", hash[i]);
    return result;
}

void ClassifyFingerprintV2(ExeFingerprint& fp) {
    fp.variant = GtaExeVariant::Unknown;
    fp.identified = false;
    fp.structuralMatch = false;
    fp.exactFingerprintVerified = false;
    fp.compatibilityApproved = false;
    fp.matchedProfileId.clear();
    if (!fp.fileReadable || !fp.peValid || !fp.sha256Computed ||
        fp.peMachine != IMAGE_FILE_MACHINE_I386) return;

    const std::string fullHash = Sha256ToString(fp.sha256.data());
    const std::string layoutHash = Sha256ToString(fp.sectionLayoutSha256.data());
    if (fp.fileSize == 14383616 && fp.peTimestamp == 0x427101CA &&
        fp.peImageSize == 0x01177000 && fp.sectionCount == 11 &&
        HasSection(fp, ".HOODLUM") && layoutHash == kHoodlumLayoutSha256) {
        fp.variant = GtaExeVariant::V10USHoodlum;
        fp.identified = true;
        fp.structuralMatch = true;
        fp.matchedProfileId = "gta-sa-1.0-us-hoodlum-family";
        fp.exactFingerprintVerified = fullHash == kHoodlumReferenceSha256;

#ifdef SKR_TEST_CERTIFICATION
        // ── TEST_ONLY: Experimental certification for controlled runtime testing ──
        // This block ONLY compiles when SKR_TEST_CERTIFICATION=1 is defined.
        // It grants compatibilityApproved to the exact canonical Hoodlum executable
        // (SHA-256 A559AA...) to enable controlled game startup testing.
        // It must NEVER be compiled into a production Release build.
        if (fp.exactFingerprintVerified && fullHash == kHoodlumReferenceSha256) {
            fp.compatibilityApproved = true;
            fp.matchedProfileId = "gta-sa-1.0-us-hoodlum-TEST_ONLY";
        }
#endif
        return;
    }
    const bool compactStructure =
        (fp.fileSize == 5189632 || fp.fileSize == 5190656) &&
        fp.peTimestamp == 0x427101CA && fp.peImageSize == 0x008B1000 &&
        fp.sectionCount == 7 && HasCompactSections(fp) &&
        (layoutHash == kCompactRetailLayoutSha256 || layoutHash == kCompactBaseLayoutSha256);
    if (compactStructure || fullHash == kCompactReferenceSha256) {
        fp.variant = GtaExeVariant::V10USCompact;
        fp.identified = true;
        fp.structuralMatch = compactStructure;
        fp.matchedProfileId = "gta-sa-1.0-us-compact-reference";
        fp.exactFingerprintVerified = fullHash == kCompactReferenceSha256;
        // Multiplayer provenance is not sufficient to certify SKR native hooks.
        return;
    }
    for (const auto& profile : kInformationalProfiles) {
        if ((profile.fileSize == 0 || profile.fileSize == fp.fileSize) &&
            profile.timestamp == fp.peTimestamp && profile.imageSize == fp.peImageSize) {
            fp.variant = profile.variant;
            fp.identified = true;
            fp.structuralMatch = true;
            fp.matchedProfileId = "legacy-informational-profile";
            return;
        }
    }
}

ExeFingerprint FingerprintExecutable(const std::wstring& exePath) {
    ExeFingerprint fp;
    HANDLE identityLock = CreateFileW(exePath.c_str(), GENERIC_READ,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (identityLock == INVALID_HANDLE_VALUE) {
        fp.parseError = GetLastError() == ERROR_FILE_NOT_FOUND ? "File not found" : "File is not readable";
        return fp;
    }
    LARGE_INTEGER length{};
    fp.fileReadable = GetFileSizeEx(identityLock, &length) != FALSE;
    if (fp.fileReadable) fp.fileSize = static_cast<uint64_t>(length.QuadPart);

    PeHeaders pe{};
    if (!ReadPeHeaders(exePath, pe)) {
        fp.parseError = "Invalid or corrupted PE32 headers";
        CloseHandle(identityLock);
        return fp;
    }
    fp.peValid = true;
    fp.peMachine = static_cast<uint16_t>(pe.machine);
    fp.peTimestamp = pe.timestamp;
    fp.peImageSize = pe.imageSize;
    fp.sectionCount = pe.sectionCount;

    std::vector<uint8_t> layout;
    layout.reserve(pe.sections.size() * 20 + 4);
    for (const auto& section : pe.sections) {
        char name[9]{};
        memcpy(name, section.Name, 8);
        fp.sectionNames.emplace_back(name, strnlen(name, 8));
        layout.insert(layout.end(), section.Name, section.Name + 8);
        const uint32_t values[] = {section.VirtualAddress, section.Misc.VirtualSize, section.Characteristics};
        for (const auto value : values) {
            layout.push_back(static_cast<uint8_t>(value));
            layout.push_back(static_cast<uint8_t>(value >> 8));
            layout.push_back(static_cast<uint8_t>(value >> 16));
            layout.push_back(static_cast<uint8_t>(value >> 24));
        }
    }
    const uint32_t count = fp.sectionCount;
    layout.push_back(static_cast<uint8_t>(count));
    layout.push_back(static_cast<uint8_t>(count >> 8));
    layout.push_back(static_cast<uint8_t>(count >> 16));
    layout.push_back(static_cast<uint8_t>(count >> 24));
    HashBytes(layout.data(), layout.size(), fp.sectionLayoutSha256);

    const std::string digest = ComputeFileSha256(exePath);
    fp.sha256Computed = ParseHex(digest, fp.sha256);
    if (!fp.sha256Computed) {
        fp.parseError = "SHA-256 could not be computed";
        CloseHandle(identityLock);
        return fp;
    }
    CloseHandle(identityLock);
    ClassifyFingerprintV2(fp);
    return fp;
}

CheckResult EvaluateExeFingerprint(const ExeFingerprint& fp, GtaExeVariant& outVariant) {
    outVariant = fp.variant;
    CheckResult result;
    const std::string hash = fp.sha256Computed ? Sha256ToString(fp.sha256.data()) : "unavailable";
    std::ostringstream detail;
    detail << "Family: " << VariantToString(fp.variant)
           << "\nMachine: " << Hex16(fp.peMachine)
           << "\nPE timestamp: " << Hex32(fp.peTimestamp)
           << "\nImage size: " << Hex32(fp.peImageSize)
           << "\nFile size: " << fp.fileSize
           << "\nSections: " << fp.sectionCount
           << "\nSection layout SHA-256: " << Sha256ToString(fp.sectionLayoutSha256.data())
           << "\nFull-file SHA-256: " << hash
           << "\nStructural match: " << (fp.structuralMatch ? "yes" : "no")
           << "\nExact fingerprint verified: " << (fp.exactFingerprintVerified ? "yes" : "no")
           << "\nSKR compatibility approved: " << (fp.compatibilityApproved ? "yes" : "no");
    result.detail = detail.str();

    if (!fp.fileReadable || !fp.peValid || !fp.sha256Computed) {
        result.status = CheckStatus::Unsupported;
        result.code = !fp.fileReadable ? "EXE_UNREADABLE" : (!fp.peValid ? "EXE_INVALID_PE" : "EXE_HASH_FAILED");
        result.summary = "GTA executable identity could not be verified";
        if (!fp.parseError.empty()) result.detail += "\nReason: " + fp.parseError;
        result.remediation = "Choose an intact, readable GTA San Andreas installation from a legitimate source.";
        return result;
    }
    if (hash == kBlockedLocalSha256) {
        result.status = CheckStatus::Unsupported;
        result.code = "EXE_BLOCKED_HASH";
        result.summary = "Hoodlum-like executable has an explicitly blocked identity";
        result.remediation = "Do not run or redistribute this file. Restore GTA San Andreas from a trusted source.";
        return result;
    }
    if (!fp.identified) {
        result.status = CheckStatus::Unsupported;
        result.code = "EXE_UNKNOWN";
        result.summary = "Unrecognized or modified GTA San Andreas executable";
        result.remediation = "Restore an unmodified supported executable from a legitimate source.";
        return result;
    }
    if (!fp.exactFingerprintVerified) {
        result.status = CheckStatus::Unsupported;
        result.code = fp.structuralMatch ? "EXE_FAMILY_UNVERIFIED" : "EXE_HASH_MISMATCH";
        result.summary = std::string("GTA SA ") + VariantToString(fp.variant) +
            " recognized, but the exact file identity is not verified";
        result.remediation = "Use an exact provenance-verified executable. Structural similarity cannot authorize SKR native hooks.";
        return result;
    }
    if (!fp.compatibilityApproved) {
        result.status = CheckStatus::Unsupported;
        result.code = "EXE_PROFILE_NOT_CERTIFIED";
        result.summary = std::string("GTA SA ") + VariantToString(fp.variant) +
            " identity verified, but not yet certified for SKR native hooks";
        result.remediation = "Use an SKR-certified executable profile after native hook compatibility has been independently validated.";
        return result;
    }
    result.status = CheckStatus::Verified;
    result.code = "EXE_APPROVED";
    result.summary = std::string("GTA SA ") + VariantToString(fp.variant) + " — verified for SKR";
    return result;
}

} // namespace skr::preflight
