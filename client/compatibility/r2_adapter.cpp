#include "compatibility/r2_adapter.h"
#include <cstring>
#include <memory>

#pragma comment(lib, "version.lib")

namespace skr::compat {

SampVersion DetectSampVersion() {
    DWORD dummy = 0;
    DWORD size = GetFileVersionInfoSizeA("samp.dll", &dummy);
    if (size == 0) return SampVersion::Unknown;

    auto buffer = std::make_unique<uint8_t[]>(size);
    if (!GetFileVersionInfoA("samp.dll", 0, size, buffer.get()))
        return SampVersion::Unknown;

    VS_FIXEDFILEINFO* fileInfo = nullptr;
    UINT len = 0;
    if (!VerQueryValueA(buffer.get(), "\\",
                        reinterpret_cast<void**>(&fileInfo), &len))
        return SampVersion::Unknown;

    DWORD major = fileInfo->dwFileVersionMS & 0xFF;
    DWORD minor = (fileInfo->dwFileVersionLS >> 16) & 0xFF;
    DWORD rc    = fileInfo->dwFileVersionLS & 0xFF;

    if (major == 3 && minor == 7 && rc == 0) return SampVersion::V037R1;
    if (major == 3 && minor == 7 && rc == 1) return SampVersion::V037R2;
    if (major == 3 && minor == 7 && rc == 2) return SampVersion::V037R3;
    if (major == 3 && minor == 8 && rc == 0) return SampVersion::V03DL;

    return SampVersion::Unknown;
}

HMODULE GetSampBase() {
    return GetModuleHandleA("samp.dll");
}

bool VerifyR2Binary(const uint8_t /*expectedSha256*/[32]) {
    // TODO: compute SHA-256 of samp.dll and compare.
    // Requires linking a hash library or using Windows CNG (BCryptHashData).
    // Not implemented until we have the canonical R2 hash.
    return false;
}

bool IsSampCefLoaded() {
    return GetModuleHandleA("cef-client.dll") != nullptr
        || GetModuleHandleA("cef.asi") != nullptr;
}

} // namespace skr::compat
