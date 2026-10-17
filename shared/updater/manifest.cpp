#include "updater/manifest.h"
#include <cstdio>
#include <cstring>

namespace skr::updater {

bool ManifestSerializer::IsPathSafe(const char* path) {
    if (!path || path[0] == '\0') return false;

    // Reject absolute paths
    if (path[0] == '/' || path[0] == '\\') return false;
    if (strlen(path) >= 2 && path[1] == ':') return false;

    // Reject path traversal
    const char* p = path;
    while (*p) {
        if (p[0] == '.' && p[1] == '.') {
            if (p[2] == '\0' || p[2] == '/' || p[2] == '\\')
                return false;
        }
        ++p;
    }

    // Reject backslashes (normalize to forward slash)
    p = path;
    while (*p) {
        if (*p == '\\') return false;
        ++p;
    }

    // Reject null bytes in the meaningful part
    if (strlen(path) == 0) return false;

    return true;
}

bool ManifestSerializer::ValidateEntry(const ManifestEntry& entry) {
    if (!IsPathSafe(entry.relativePath)) return false;
    if (entry.sizeBytes == 0 && entry.action != FileAction::Delete) return false;

    // Check hash is not all zeros for add/update
    if (entry.action != FileAction::Delete) {
        bool allZero = true;
        for (size_t i = 0; i < kSha256Size; ++i) {
            if (entry.sha256[i] != 0) { allZero = false; break; }
        }
        if (allZero) return false;
    }

    return true;
}

// Binary format: header + entries
// Header: u32 manifestVersion, version[32], channel[16], platform[16],
//         arch[16], keyId[64], u64 timestamp, u32 minVer[3], u32 entryCount
// Entry:  path[260], sha256[32], u64 size, u8 action, u8 executable
//
// Production would use signed JSON; this is for development/testing.

bool ManifestSerializer::Serialize(const ReleaseManifest& manifest,
                                   std::vector<uint8_t>& out) {
    out.clear();

    auto writeBytes = [&](const void* src, size_t len) {
        auto p = reinterpret_cast<const uint8_t*>(src);
        out.insert(out.end(), p, p + len);
    };
    auto writeU32 = [&](uint32_t v) { writeBytes(&v, 4); };
    auto writeU64 = [&](uint64_t v) { writeBytes(&v, 8); };

    writeU32(manifest.manifestVersion);
    writeBytes(manifest.version, kMaxVersionLen);
    writeBytes(manifest.channel, kMaxChannelLen);
    writeBytes(manifest.platform, kMaxPlatformLen);
    writeBytes(manifest.architecture, kMaxPlatformLen);
    writeBytes(manifest.keyId, kMaxKeyIdLen);
    writeU64(manifest.timestamp);
    writeU32(manifest.minSupportedVersion[0]);
    writeU32(manifest.minSupportedVersion[1]);
    writeU32(manifest.minSupportedVersion[2]);
    writeU32(static_cast<uint32_t>(manifest.entries.size()));

    for (auto& entry : manifest.entries) {
        if (!ValidateEntry(entry)) return false;
        writeBytes(entry.relativePath, kMaxPathLen);
        writeBytes(entry.sha256, kSha256Size);
        writeU64(entry.sizeBytes);
        uint8_t act = static_cast<uint8_t>(entry.action);
        writeBytes(&act, 1);
        uint8_t exe = entry.executable ? 1 : 0;
        writeBytes(&exe, 1);
    }

    return true;
}

bool ManifestSerializer::Deserialize(const uint8_t* data, size_t len,
                                     ReleaseManifest& out) {
    out.Clear();

    constexpr size_t headerSize = 4 + kMaxVersionLen + kMaxChannelLen +
                                  kMaxPlatformLen + kMaxPlatformLen +
                                  kMaxKeyIdLen + 8 + 12 + 4;
    if (len < headerSize) return false;

    size_t pos = 0;
    auto readBytes = [&](void* dst, size_t n) {
        if (pos + n > len) return false;
        memcpy(dst, data + pos, n);
        pos += n;
        return true;
    };
    auto readU32 = [&](uint32_t& v) { return readBytes(&v, 4); };
    auto readU64 = [&](uint64_t& v) { return readBytes(&v, 8); };

    if (!readU32(out.manifestVersion)) return false;
    if (out.manifestVersion != kManifestVersion) return false;

    if (!readBytes(out.version, kMaxVersionLen)) return false;
    if (!readBytes(out.channel, kMaxChannelLen)) return false;
    if (!readBytes(out.platform, kMaxPlatformLen)) return false;
    if (!readBytes(out.architecture, kMaxPlatformLen)) return false;
    if (!readBytes(out.keyId, kMaxKeyIdLen)) return false;
    if (!readU64(out.timestamp)) return false;
    if (!readU32(out.minSupportedVersion[0])) return false;
    if (!readU32(out.minSupportedVersion[1])) return false;
    if (!readU32(out.minSupportedVersion[2])) return false;

    uint32_t entryCount = 0;
    if (!readU32(entryCount)) return false;
    if (entryCount > 10000) return false;

    constexpr size_t entrySize = kMaxPathLen + kSha256Size + 8 + 1 + 1;
    if (pos + static_cast<size_t>(entryCount) * entrySize > len) return false;

    out.entries.resize(entryCount);
    for (uint32_t i = 0; i < entryCount; ++i) {
        auto& e = out.entries[i];
        if (!readBytes(e.relativePath, kMaxPathLen)) return false;
        if (!readBytes(e.sha256, kSha256Size)) return false;
        if (!readU64(e.sizeBytes)) return false;
        uint8_t act = 0;
        if (!readBytes(&act, 1)) return false;
        if (act > 2) return false;
        e.action = static_cast<FileAction>(act);
        uint8_t exe = 0;
        if (!readBytes(&exe, 1)) return false;
        e.executable = (exe != 0);

        if (!ValidateEntry(e)) return false;
    }

    return true;
}

} // namespace skr::updater
