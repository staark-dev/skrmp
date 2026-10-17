#define WIN32_LEAN_AND_MEAN
#include "compatibility/compat_profile.h"
#include <windows.h>
#include <bcrypt.h>
#include <cstdio>
#include <ctime>

#pragma comment(lib, "bcrypt.lib")

namespace skr::compat {

void Sha256ToHex(const uint8_t hash[kSha256Len], char out[kMaxHashHex]) {
    for (int i = 0; i < 32; ++i)
        snprintf(out + i * 2, 3, "%02x", hash[i]);
    out[64] = '\0';
}

static bool ComputeSha256(const wchar_t* path, uint8_t out[kSha256Len], uint64_t& sizeOut) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER fileSize{};
    GetFileSizeEx(file, &fileSize);
    sizeOut = static_cast<uint64_t>(fileSize.QuadPart);

    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = false;

    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0) {
        if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
            uint8_t buf[8192];
            DWORD bytesRead = 0;
            ok = true;
            while (ReadFile(file, buf, sizeof(buf), &bytesRead, nullptr) && bytesRead > 0) {
                if (BCryptHashData(hash, buf, bytesRead, 0) != 0) { ok = false; break; }
            }
            if (ok) ok = (BCryptFinishHash(hash, out, kSha256Len, 0) == 0);
            BCryptDestroyHash(hash);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }

    CloseHandle(file);
    return ok;
}

ProfileBuilder::ProfileBuilder() {
    profile_.Clear();
}

void ProfileBuilder::SetTarget(const char* config, const char* clientBuild) {
    strncpy(profile_.targetConfig, config, sizeof(profile_.targetConfig) - 1);
    strncpy(profile_.clientBuild, clientBuild, sizeof(profile_.clientBuild) - 1);
    profile_.createdAt = static_cast<uint64_t>(std::time(nullptr));
    profile_.lastUpdated = profile_.createdAt;
}

void ProfileBuilder::CollectSystemInfo() {
    OSVERSIONINFOEXW osvi{};
    osvi.dwOSVersionInfoSize = sizeof(osvi);
    // Use RtlGetVersion to avoid manifest requirement
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    auto ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll) {
        auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
        if (fn) fn(reinterpret_cast<PRTL_OSVERSIONINFOW>(&osvi));
    }
    snprintf(profile_.osVersion, sizeof(profile_.osVersion),
             "Windows %lu.%lu.%lu",
             osvi.dwMajorVersion, osvi.dwMinorVersion, osvi.dwBuildNumber);

    // CPU name from registry
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                      0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD size = sizeof(profile_.cpuName);
        RegQueryValueExA(hKey, "ProcessorNameString", nullptr, nullptr,
                         reinterpret_cast<LPBYTE>(profile_.cpuName), &size);
        RegCloseKey(hKey);
    }

    // GPU and driver from registry (first display adapter)
    HKEY hVideoKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}\\0000",
                      0, KEY_READ, &hVideoKey) == ERROR_SUCCESS) {
        DWORD size = sizeof(profile_.gpuName);
        RegQueryValueExA(hVideoKey, "DriverDesc", nullptr, nullptr,
                         reinterpret_cast<LPBYTE>(profile_.gpuName), &size);
        size = sizeof(profile_.gpuDriver);
        RegQueryValueExA(hVideoKey, "DriverVersion", nullptr, nullptr,
                         reinterpret_cast<LPBYTE>(profile_.gpuDriver), &size);
        RegCloseKey(hVideoKey);
    }
}

bool ProfileBuilder::AddModuleHash(const char* name, const char* path) {
    if (profile_.moduleCount >= kMaxModuleEntries) return false;

    auto& m = profile_.modules[profile_.moduleCount];
    strncpy(m.name, name, sizeof(m.name) - 1);
    strncpy(m.path, path, sizeof(m.path) - 1);

    wchar_t wpath[kMaxPathLen]{};
    MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, kMaxPathLen);

    uint64_t size = 0;
    if (!ComputeSha256(wpath, m.sha256, size)) return false;
    m.sizeBytes = size;
    m.verified = true;

    // Read file version info
    DWORD dummy = 0;
    DWORD versionSize = GetFileVersionInfoSizeA(path, &dummy);
    if (versionSize > 0) {
        auto buf = new uint8_t[versionSize];
        if (GetFileVersionInfoA(path, 0, versionSize, buf)) {
            VS_FIXEDFILEINFO* ffi = nullptr;
            UINT ffiLen = 0;
            if (VerQueryValueA(buf, "\\", reinterpret_cast<void**>(&ffi), &ffiLen) && ffi) {
                snprintf(m.fileVersion, kMaxVersionStr, "%u.%u.%u.%u",
                         HIWORD(ffi->dwFileVersionMS), LOWORD(ffi->dwFileVersionMS),
                         HIWORD(ffi->dwFileVersionLS), LOWORD(ffi->dwFileVersionLS));
            }
        }
        delete[] buf;
    }

    ++profile_.moduleCount;
    return true;
}

void ProfileBuilder::AddTestResult(const char* name, const char* category,
                                   TestResult result, uint32_t iterations,
                                   uint32_t failures, const char* notes) {
    if (profile_.testCount >= kMaxTestEntries) return;

    auto& t = profile_.tests[profile_.testCount];
    strncpy(t.name, name, sizeof(t.name) - 1);
    strncpy(t.category, category, sizeof(t.category) - 1);
    t.result = result;
    t.iterations = iterations;
    t.failures = failures;
    if (notes) strncpy(t.notes, notes, sizeof(t.notes) - 1);
    ++profile_.testCount;

    profile_.lastUpdated = static_cast<uint64_t>(std::time(nullptr));
}

void ProfileBuilder::EvaluateStatus() {
    if (profile_.testCount == 0) {
        profile_.status = ProfileStatus::Unchecked;
        return;
    }

    bool allPassed = true;
    bool anyFailed = false;
    bool allRun = true;

    for (uint32_t i = 0; i < profile_.testCount; ++i) {
        auto& t = profile_.tests[i];
        if (t.result == TestResult::NotRun) allRun = false;
        if (t.result == TestResult::Failed) { anyFailed = true; allPassed = false; }
        if (t.result != TestResult::Passed) allPassed = false;
    }

    // Check all modules verified
    bool modulesOk = true;
    for (uint32_t i = 0; i < profile_.moduleCount; ++i) {
        if (!profile_.modules[i].verified) { modulesOk = false; break; }
    }

    if (anyFailed) {
        profile_.status = ProfileStatus::Failed;
    } else if (allPassed && allRun && modulesOk) {
        profile_.status = ProfileStatus::Certified;
    } else {
        profile_.status = ProfileStatus::Pending;
    }
}

bool ProfileBuilder::SerializeToFile(const wchar_t* path) const {
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0,
                              nullptr, CREATE_ALWAYS, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    DWORD written = 0;
    WriteFile(file, &profile_, sizeof(CompatibilityProfile), &written, nullptr);
    CloseHandle(file);
    return written == sizeof(CompatibilityProfile);
}

bool ProfileBuilder::DeserializeFromFile(const wchar_t* path) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    DWORD bytesRead = 0;
    ReadFile(file, &profile_, sizeof(CompatibilityProfile), &bytesRead, nullptr);
    CloseHandle(file);

    if (bytesRead != sizeof(CompatibilityProfile)) {
        profile_.Clear();
        return false;
    }
    if (profile_.version != 1) {
        profile_.Clear();
        return false;
    }
    return true;
}

} // namespace skr::compat
