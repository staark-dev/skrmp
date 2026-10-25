#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>
#include <tlhelp32.h>
#include "operation213/vehicle_store_relocation.h"

namespace skr::operation213 {
namespace {

constexpr uintptr_t kImageBase = 0x00400000;
constexpr uintptr_t kEntryPoint = 0x00824570;
constexpr DWORD kPeTimestamp = 0x427101CA;
constexpr DWORD kImageSize = 0x01177000;
constexpr std::array<uint8_t, 32> kExecutableSha256{
    0xA5,0x59,0xAA,0x77,0x2F,0xD1,0x36,0x37,0x91,0x55,0xEF,0xA7,0x1F,0x00,0xC4,0x7A,
    0xAD,0x34,0xBB,0xFE,0xAE,0x61,0x96,0xB0,0xFE,0x10,0x47,0xD0,0x64,0x5C,0xBD,0x26
};

struct Site {
    uintptr_t address;
    uint8_t size;
    uint8_t operandOffset;
    std::array<uint8_t, 6> original;
    std::array<uint8_t, 6> replacement;
};

std::atomic<Status> g_status{Status::NotStarted};
void* g_allocation = nullptr;
uint32_t g_patchedSites = 0;

void Log(const char* format, ...) {
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return;
    wcscpy_s(slash + 1, MAX_PATH - static_cast<size_t>(slash + 1 - path), L"SKR\\operation213.log");
    FILE* file = _wfopen(path, L"a");
    if (!file) return;
    std::fprintf(file, "[%lu] ", GetTickCount());
    va_list args;
    va_start(args, format);
    std::vfprintf(file, format, args);
    va_end(args);
    std::fputc('\n', file);
    std::fclose(file);
}

bool ReadExact(uintptr_t address, void* output, size_t size) {
    SIZE_T read = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address),
                             output, size, &read) && read == size;
}

bool HashExecutable(std::array<uint8_t, 32>& digest) {
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return false;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectSize = 0;
    DWORD resultSize = 0;
    bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
              BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                  reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &resultSize, 0) >= 0;
    std::vector<uint8_t> object(objectSize);
    if (ok) ok = BCryptCreateHash(algorithm, &hash, object.data(), objectSize, nullptr, 0, 0) >= 0;
    std::array<uint8_t, 64 * 1024> buffer{};
    while (ok) {
        DWORD bytes = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes, nullptr)) {
            ok = false;
            break;
        }
        if (!bytes) break;
        ok = BCryptHashData(hash, buffer.data(), bytes, 0) >= 0;
    }
    if (ok) ok = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(file);
    return ok;
}

bool SupportedExecutable() {
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    if (reinterpret_cast<uintptr_t>(base) != kImageBase) {
        Log("identity base mismatch observed=%p", base); return false;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) { Log("identity DOS signature mismatch"); return false; }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        nt->FileHeader.TimeDateStamp != kPeTimestamp || nt->OptionalHeader.SizeOfImage != kImageSize ||
        kImageBase + nt->OptionalHeader.AddressOfEntryPoint != kEntryPoint) {
        Log("identity PE mismatch sig=%08lX machine=%04X stamp=%08lX size=%08lX entry=%08lX",
            nt->Signature, nt->FileHeader.Machine, nt->FileHeader.TimeDateStamp,
            nt->OptionalHeader.SizeOfImage, kImageBase + nt->OptionalHeader.AddressOfEntryPoint);
        return false;
    }
    std::array<uint8_t, 32> digest{};
    if (!HashExecutable(digest)) { Log("identity SHA-256 read failed error=%lu", GetLastError()); return false; }
    if (digest != kExecutableSha256) {
        char text[65]{};
        for (size_t i = 0; i < digest.size(); ++i) std::sprintf(text + i * 2, "%02X", digest[i]);
        Log("identity SHA-256 mismatch observed=%s", text);
        return false;
    }
    return true;
}

bool PrimaryThreadSuspended(DWORD threadId) {
    if (!threadId || threadId == GetCurrentThreadId()) return false;
    HANDLE thread = OpenThread(THREAD_QUERY_INFORMATION, FALSE, threadId);
    if (!thread) return false;
    using NtQueryInformationThreadFn = LONG (NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
    auto query = reinterpret_cast<NtQueryInformationThreadFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
    ULONG suspendCount = 0;
    PVOID startAddress = nullptr;
    bool ok = query && query(thread, 35, &suspendCount, sizeof(suspendCount), nullptr) >= 0 &&
              query(thread, 9, &startAddress, sizeof(startAddress), nullptr) >= 0 &&
              suspendCount > 0 && reinterpret_cast<uintptr_t>(startAddress) == kEntryPoint;
    CloseHandle(thread);
    return ok;
}

std::array<Site, 12> BuildPlan(uintptr_t countAddress, uintptr_t descriptorAddress) {
    std::array<Site, 12> sites{{
        {0x4C64EC,5,1,{0xA1,0x50,0xF6,0xB1,0x00,0},{0xA1,0x50,0xF6,0xB1,0x00,0}},
        {0x4C6507,5,1,{0xA1,0x50,0xF6,0xB1,0x00,0},{0xA1,0x50,0xF6,0xB1,0x00,0}},
        {0x4C6602,6,2,{0x89,0x2D,0x50,0xF6,0xB1,0x00},{0x89,0x2D,0x50,0xF6,0xB1,0x00}},
        {0x4C6770,5,1,{0xA1,0x50,0xF6,0xB1,0x00,0},{0xA1,0x50,0xF6,0xB1,0x00,0}},
        {0x4C6785,5,1,{0xA3,0x50,0xF6,0xB1,0x00,0},{0xA3,0x50,0xF6,0xB1,0x00,0}},
        {0x4C6851,6,2,{0x89,0x3D,0x50,0xF6,0xB1,0x00},{0x89,0x3D,0x50,0xF6,0xB1,0x00}},
        {0x84BCD0,5,1,{0xB9,0x50,0xF6,0xB1,0x00,0},{0xB9,0x50,0xF6,0xB1,0x00,0}},
        {0x8562A0,5,1,{0xB9,0x50,0xF6,0xB1,0x00,0},{0xB9,0x50,0xF6,0xB1,0x00,0}},
        {0x4C64F7,5,1,{0xBF,0x54,0xF6,0xB1,0x00,0},{0xBF,0x54,0xF6,0xB1,0x00,0}},
        {0x4C677E,6,2,{0x81,0xC6,0x54,0xF6,0xB1,0x00},{0x81,0xC6,0x54,0xF6,0xB1,0x00}},
        {0x4C5F5B,5,1,{0x68,0xD4,0x00,0x00,0x00,0},{0x68,0xD4,0x00,0x00,0x00,0}},
        {0x4C6375,5,1,{0x68,0xD4,0x00,0x00,0x00,0},{0x68,0xD4,0x00,0x00,0x00,0}},
    }};
    for (size_t i = 0; i < 8; ++i)
        std::memcpy(sites[i].replacement.data() + sites[i].operandOffset, &countAddress, sizeof(uint32_t));
    for (size_t i = 8; i < 10; ++i)
        std::memcpy(sites[i].replacement.data() + sites[i].operandOffset, &descriptorAddress, sizeof(uint32_t));
    const uint32_t capacity = kRelocatedCapacity;
    for (size_t i = 10; i < 12; ++i)
        std::memcpy(sites[i].replacement.data() + sites[i].operandOffset, &capacity, sizeof(capacity));
    return sites;
}

void Fail(Status status, const char* reason) {
    g_status.store(status);
    Log("FAIL status=%lu reason=%s patched=%lu", static_cast<unsigned long>(status), reason,
        static_cast<unsigned long>(g_patchedSites));
}

} // namespace

DWORD FindSuspendedPrimaryThread() noexcept {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    const DWORD pid = GetCurrentProcessId();
    DWORD found = 0;
    uint32_t matches = 0;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != pid || !PrimaryThreadSuspended(entry.th32ThreadID)) continue;
        found = entry.th32ThreadID;
        ++matches;
    }
    CloseHandle(snapshot);
    Log("primary thread scan matches=%lu tid=%lu", static_cast<unsigned long>(matches), found);
    return matches == 1 ? found : 0;
}

bool PartiallyApplied() noexcept {
    const Status status = g_status.load();
    return g_patchedSites != 0 && status != Status::Prepared;
}

bool Prepare(DWORD primaryThreadId) noexcept {
    Status expected = Status::NotStarted;
    if (!g_status.compare_exchange_strong(expected, Status::Preparing)) return expected == Status::Prepared;
    if (!primaryThreadId) { Fail(Status::InvalidPrimaryThread, "missing primary thread id"); return false; }
    if (!PrimaryThreadSuspended(primaryThreadId)) {
        Fail(Status::PrimaryThreadNotSuspended, "primary thread is not the suspended GTA entry thread"); return false;
    }
    if (!SupportedExecutable()) { Fail(Status::UnsupportedExecutable, "unsupported executable identity"); return false; }
    g_allocation = VirtualAlloc(nullptr, kAllocationSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!g_allocation) { Fail(Status::AllocationFailed, "VirtualAlloc failed"); return false; }
    const uintptr_t countAddress = reinterpret_cast<uintptr_t>(g_allocation);
    const uintptr_t descriptorAddress = countAddress + sizeof(uint32_t);
    if ((descriptorAddress & 0xF) != (0xB1F654 & 0xF)) {
        VirtualFree(g_allocation, 0, MEM_RELEASE); g_allocation = nullptr;
        Fail(Status::AllocationFailed, "descriptor alignment mismatch"); return false;
    }
    auto sites = BuildPlan(countAddress, descriptorAddress);
    std::array<uint8_t, 6> observed{};
    for (const auto& site : sites) {
        if (!ReadExact(site.address, observed.data(), site.size) ||
            std::memcmp(observed.data(), site.original.data(), site.size) != 0) {
            VirtualFree(g_allocation, 0, MEM_RELEASE); g_allocation = nullptr;
            Fail(Status::UnexpectedOriginalBytes, "original instruction verification failed"); return false;
        }
    }
    for (size_t i = 0; i < sites.size(); ++i) {
        auto& site = sites[i];
        DWORD oldProtection = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(site.address), site.size,
                            PAGE_EXECUTE_READWRITE, &oldProtection)) {
            Fail(Status::ProtectionFailed, "VirtualProtect write enable failed"); return false;
        }
        SIZE_T written = 0;
        bool wrote = WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(site.address),
                                        site.replacement.data(), site.size, &written) && written == site.size;
        DWORD ignored = 0;
        bool restored = VirtualProtect(reinterpret_cast<void*>(site.address), site.size,
                                       oldProtection, &ignored) != FALSE;
        if (!wrote) { Fail(Status::WriteFailed, "instruction write failed"); return false; }
        ++g_patchedSites;
        if (!restored) { Fail(Status::ProtectionRestoreFailed, "page protection restore failed"); return false; }
    }
    if (!FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(kImageBase), kImageSize)) {
        Fail(Status::FlushFailed, "FlushInstructionCache failed"); return false;
    }
    for (const auto& site : sites) {
        if (!ReadExact(site.address, observed.data(), site.size) ||
            std::memcmp(observed.data(), site.replacement.data(), site.size) != 0) {
            Fail(Status::ReadbackMismatch, "modified instruction readback failed"); return false;
        }
    }
    g_status.store(Status::Prepared);
    Log("PASS allocation=%p size=0x%IX count=%p descriptors=%p sites=%lu",
        g_allocation, kAllocationSize, reinterpret_cast<void*>(countAddress),
        reinterpret_cast<void*>(descriptorAddress), static_cast<unsigned long>(g_patchedSites));
    return true;
}

State GetState() noexcept {
    const uintptr_t allocation = reinterpret_cast<uintptr_t>(g_allocation);
    return {g_status.load(), allocation, allocation, allocation ? allocation + sizeof(uint32_t) : 0,
            static_cast<uint32_t>(kAllocationSize), g_patchedSites};
}

bool VerifyExecutableIdentity() noexcept { return SupportedExecutable(); }

bool VerifyPatchedSites() noexcept {
    const State state = GetState();
    if (state.status != Status::Prepared || !state.countAddress || !state.descriptorAddress ||
        state.patchedSites != 12) return false;
    const auto sites = BuildPlan(state.countAddress, state.descriptorAddress);
    std::array<uint8_t, 6> observed{};
    for (const auto& site : sites) {
        if (!ReadExact(site.address, observed.data(), site.size) ||
            std::memcmp(observed.data(), site.replacement.data(), site.size) != 0) return false;
    }
    return true;
}


} // namespace skr::operation213
