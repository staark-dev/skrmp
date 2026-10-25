// ─────────────────────────────────────────────────────────────────────
// Operation 215 — native custom vehicle registration via SAMP\CUSTOM.IMG
// ─────────────────────────────────────────────────────────────────────
// Adapted from SKR-EXP-001 client/operation215/native_vehicle_registration.cpp
// (asset profile DUAL, certified D5 + D3.4, evidence op215-d5-d34-certified).
// Function bodies keep EXP-001 names, constants and checks; see the header
// for what was removed and how failures are handled differently.
// ─────────────────────────────────────────────────────────────────────

#include "operation215/native_vehicle_registration.h"
#include "operation215/op215_preconditions.h"
#include "operation213/vehicle_store_relocation.h"

#include <bcrypt.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace skr::operation215 {
namespace {

constexpr uintptr_t kAddVehicleModel = 0x004C6770;
constexpr uintptr_t kModelInfoPtrs = 0x00A9B0C8;
constexpr uintptr_t kGameState = 0x00C8D4C0;
constexpr uintptr_t kVehicleVtable = 0x0085C5C8;
constexpr uint32_t kModelCapacity = 20000;
constexpr uint32_t kOriginalFirstModel = 400;
constexpr uint32_t kOriginalLastModel = 611;
constexpr uint32_t kNewModel = 612;
constexpr uint32_t kModel613 = 613;
constexpr uint32_t kModel614 = 614;   // descriptor 214 reserved (Audi RS6, not certified)
constexpr uint32_t kCapacity = operation213::kRelocatedCapacity;
constexpr size_t kDescriptorSize = operation213::kDescriptorSize;
constexpr std::array<uint8_t, 32> kSampR5Sha256{
    0xB7,0x2B,0x5D,0xBE,0x72,0x5F,0x81,0x86,0x4C,0xA3,0xF7,0x8B,0xC7,0x06,0x3B,0xDA,
    0x56,0xCC,0x05,0xFC,0x71,0x88,0xAF,0x82,0x2F,0xA7,0xA7,0x54,0x43,0x25,0x53,0xA2
};
static_assert(kCapacity == 215);

constexpr uintptr_t kLoadCdDirectory = 0x005B6170;
constexpr uintptr_t kKeyGenUppercase = 0x0053CF30;
constexpr uintptr_t kSetTexDictionary = 0x004C4B40;
constexpr uintptr_t kFindTxdSlot = 0x00731850;
constexpr uintptr_t kGetModelInfo = 0x004C5940;
constexpr uintptr_t kGetHandlingId = 0x006F4FD0;
constexpr uintptr_t kHandlingManager = 0x00C2B9C8;
constexpr uintptr_t kTxdPoolPointer = 0x00C8800C;
constexpr uintptr_t kStreamingInfo = 0x008E4CC0;
constexpr uintptr_t kRequestModel = 0x004087E0;
constexpr uintptr_t kStreamingBufferSize = 0x008E4CA8; // CStreaming::ms_streamingBufferSize (read-only)
constexpr uintptr_t kStreamingMemoryAvailable = 0x008A5A80;
constexpr uintptr_t kStreamingMemoryUsed = 0x008E4CB4;
constexpr uint32_t kStreamingCount = 26316;
constexpr uint32_t kTxdBase = 20000;
constexpr uint32_t kSourceModel = 560;
// D5 dual profile: evidence/op215-d5-dual/CUSTOM.IMG
constexpr char kAssetName[] = "skr_xc60";
constexpr char kAssetProfile[] = "XC60_M4GTS";
constexpr uint32_t kExpectedTxdSector = 4;
constexpr uint32_t kExpectedTxdSectors = 773;
constexpr uint32_t kExpectedDffSector = 777;
constexpr uint32_t kExpectedDffSectors = 4208;
constexpr char kAsset613Name[] = "skr_m4gts";
constexpr uint32_t kExpected613TxdSector = 4985;
constexpr uint32_t kExpected613TxdSectors = 1332;
constexpr uint32_t kExpected613DffSector = 6317;
constexpr uint32_t kExpected613DffSectors = 3728;
// Vanilla SA-MP CUSTOM.IMG entry, resolved natively by the same scan.
constexpr uint32_t kPlaceholderTxdStream = 23058;
constexpr int kPlaceholderTxdSlot = 3058;
constexpr uint32_t kPlaceholderTxdSector = 2;
constexpr uint32_t kPlaceholderTxdSectors = 2;
constexpr uint8_t kCustomImageId = 0;
constexpr int kTxdStreamSlots = 5000; // streaming IDs 20000..24999
constexpr size_t kMaxRecordedChanges = 8;
constexpr std::array<uint8_t, 7> kLoadCdDirectoryOriginal{0x83,0xEC,0x3C,0x8B,0x44,0x24,0x40};
constexpr std::array<uint8_t, 32> kCustomImgSha256{ // C9E4FCE6...FF3B1FDB, 20,572,160 bytes
    0xC9,0xE4,0xFC,0xE6,0x61,0xF3,0xF9,0x6F,0x56,0xC6,0xFB,0x16,0x58,0xD1,0x50,0xC4,
    0x1F,0x30,0x22,0x57,0xA7,0x0E,0xDB,0xDC,0x85,0x54,0x69,0x7F,0xFF,0x3B,0x1F,0xDB
};
constexpr int kRequestFlags = 0x12;            // GAME_REQUIRED | PRIORITY_REQUEST
constexpr ULONGLONG kLoadTimeoutMs = 20000;

#pragma pack(push, 1)
struct StreamingInfo {
    int16_t next;
    int16_t previous;
    int16_t nextOnCd;
    uint8_t flags;
    uint8_t imageId;
    uint32_t sector;
    uint32_t sectors;
    uint8_t loadState;
    uint8_t padding[3];
};
#pragma pack(pop)
static_assert(sizeof(StreamingInfo) == 0x14);

struct StreamAssociation { uint8_t imageId; uint32_t sector; uint32_t sectors; };
struct TxdDef { void* dictionary; uint16_t refs; int16_t parent; uint32_t hash; };
struct TxdPool { TxdDef* objects; uint8_t* byteMap; int size; int firstFree; bool owns; bool field11; };

using LoadCdDirectoryFn = void (__cdecl*)(void*, int);

struct ModelSlot {
    uint32_t model;
    const char* name;
    int txdSlot;
    Resource resource;
    bool queued;
    ULONGLONG requestStarted;
};

std::atomic<Stage> g_stage{Stage::NotStarted};
DWORD g_primaryThreadId = 0;
std::array<uint8_t, kCapacity * kDescriptorSize> g_descriptorsBefore{};
std::array<uint32_t, kModelCapacity> g_modelPointersBefore{};
LoadCdDirectoryFn g_originalLoadCdDirectory = nullptr;
void* g_directoryTrampoline = nullptr;
int g_vehicleTxdSlot = -1;
int g_placeholderSlotBeforeScan = -2;
std::atomic<uint32_t> g_customScanCalls{0};
std::array<StreamAssociation, kStreamingCount> g_associationsBeforeScan{};
std::array<StreamAssociation, kStreamingCount> g_associationsBeforeRequest{};
bool g_requestBaselineCaptured = false;
std::array<ModelSlot, 3> g_models{{
    {kNewModel, kAssetName, -1, Resource::Unknown, false, 0},
    {kModel613, kAsset613Name, -1, Resource::Unknown, false, 0},
    {kModel614, "skr_rs6c8", -1, Resource::Missing, false, 0},
}};
constexpr size_t kMappedModels = 2;   // 612, 613

// PLAYER.IMG (pinned): verified and its VER2 directory loaded before resume.
constexpr uintptr_t kPlayerImgIndex = 0x00BC12F8;   // CClothes::Init stores AddImageToList result
constexpr uintptr_t kStreamingFiles = 0x008E48D8;   // CStreamingFile[8], stride 0x30
constexpr size_t kStreamingFileStride = 0x30;
constexpr size_t kStreamingFileNameSize = 0x28;
constexpr size_t kStreamingFileScanFlag = 0x28;     // bNotPlayerImg
constexpr uintptr_t kPlayerTxdSlots = 0x00C88004;   // int16[4], CTxdStore::Initialise
constexpr uint64_t kPlayerImgSize = 66738176;
constexpr std::array<uint8_t, 32> kPlayerImgSha256{
    0xBD,0x6D,0x43,0xC0,0x65,0xFD,0x77,0x67,0x95,0x8C,0x5C,0x36,0x3E,0x67,0xFD,0xAA,
    0x92,0xF2,0x03,0xD7,0xC7,0x66,0x41,0x95,0x24,0xD9,0xF2,0x20,0xEE,0xC9,0x82,0x56
};
std::vector<ImgDirEntry> g_playerDirectory;
uint32_t g_playerArchiveSectors = 0;

void Log(const char* format, ...) {
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return;
    wcscpy_s(slash + 1, MAX_PATH - static_cast<size_t>(slash + 1 - path), L"SKR");
    CreateDirectoryW(path, nullptr);
    wcscat_s(path, L"\\op215.log");
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
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), output, size, &read) &&
        read == size;
}

bool HashFile(const wchar_t* path, std::array<uint8_t, 32>& digest) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
              BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
    std::vector<uint8_t> buffer(64 * 1024);
    while (ok) {
        DWORD bytes = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes, nullptr)) { ok = false; break; }
        if (!bytes) break;
        ok = BCryptHashData(hash, buffer.data(), bytes, 0) >= 0;
    }
    if (ok) ok = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(file);
    return ok;
}

bool VerifySampR5() {
    HMODULE samp = GetModuleHandleW(L"samp.dll");
    if (!samp) return false;
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(samp, path, MAX_PATH)) return false;
    std::array<uint8_t, 32> digest{};
    return HashFile(path, digest) && digest == kSampR5Sha256;
}

std::wstring DirectoryOf(HMODULE module) {
    wchar_t path[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(module, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return {};
    std::wstring s(path, n);
    const size_t slash = s.find_last_of(L'\\');
    return slash == std::wstring::npos ? std::wstring{} : s.substr(0, slash + 1);
}

// SKR Platform adaptation: the file GTA opens as SAMP\CUSTOM.IMG. In
// TEST_CERTIFICATION builds the VFS (samp_vfs.cpp TryRedirect) serves
// <dll dir>\samp\SAMP\custom.img when present (case-insensitive NTFS lookup).
std::wstring ActiveCustomArchivePath() {
#ifdef SKR_TEST_CERTIFICATION
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ActiveCustomArchivePath), &self);
    const std::wstring bundled = DirectoryOf(self) + L"samp\\SAMP\\custom.img";
    if (GetFileAttributesW(bundled.c_str()) != INVALID_FILE_ATTRIBUTES) return bundled;
#endif
    return DirectoryOf(nullptr) + L"SAMP\\custom.img";
}

bool VerifyCustomArchiveHash() {
    const std::wstring path = ActiveCustomArchivePath();
    std::array<uint8_t, 32> digest{};
    const bool hashed = HashFile(path.c_str(), digest);
    char text[65]{};
    for (size_t i = 0; hashed && i < digest.size(); ++i) std::sprintf(text + i * 2, "%02X", digest[i]);
    Log("OP215_D2B0 archive=%ls sha256=%s expected=C9E4FCE6..FF3B1FDB", path.c_str(), hashed ? text : "<unreadable>");
    return hashed && digest == kCustomImgSha256;
}

uintptr_t Descriptor(const operation213::State& state, uint32_t index) {
    return state.descriptorAddress + static_cast<uintptr_t>(index) * kDescriptorSize;
}

bool OriginalsMapped(const operation213::State& state) {
    const auto* models = reinterpret_cast<uintptr_t*>(kModelInfoPtrs);
    for (uint32_t model = kOriginalFirstModel; model <= kOriginalLastModel; ++model) {
        if (models[model] != Descriptor(state, model - kOriginalFirstModel)) return false;
    }
    return true;
}

bool DescriptorUnused(uintptr_t descriptor) {
    const auto* models = reinterpret_cast<uintptr_t*>(kModelInfoPtrs);
    for (uint32_t i = 0; i < kModelCapacity; ++i) if (models[i] == descriptor) return false;
    return true;
}

D0Snapshot Observe() {
    D0Snapshot s{};
    const operation213::State state = operation213::GetState();
    s.executableIdentity = operation213::VerifyExecutableIdentity();
    s.relocationPrepared = state.status == operation213::Status::Prepared &&
        state.allocationSize == operation213::kAllocationSize && state.patchedSites == 12;
    s.patchSites = operation213::VerifyPatchedSites();
    s.primaryThread = g_primaryThreadId != 0 && GetCurrentThreadId() == g_primaryThreadId;
    s.sampR5 = VerifySampR5();
    if (!s.relocationPrepared) return s;

    uint32_t count = 0;
    if (!ReadExact(state.countAddress, &count, sizeof(count))) return s;
    s.vehicleCount = count;
    for (uint32_t i = 0; i < kCapacity; ++i) {
        uintptr_t vtable = 0;
        if (ReadExact(Descriptor(state, i), &vtable, sizeof(vtable)) && vtable == kVehicleVtable) ++s.constructed;
    }
    const auto* models = reinterpret_cast<uintptr_t*>(kModelInfoPtrs);
    s.model612Null = models[612] == 0;
    s.model613Null = models[613] == 0;
    s.model614Null = models[614] == 0;
    s.descriptor212Unused = DescriptorUnused(Descriptor(state, 212));
    s.descriptor213Unused = DescriptorUnused(Descriptor(state, 213));
    s.descriptor214Unused = DescriptorUnused(Descriptor(state, 214));
    s.model400Correct = models[400] == Descriptor(state, 0);
    s.model560Correct = models[560] == Descriptor(state, 160);
    s.model611Correct = models[611] == Descriptor(state, 211);
    s.originalsIntact = OriginalsMapped(state);
    return s;
}

bool CaptureBefore(const operation213::State& state) {
    return ReadExact(state.descriptorAddress, g_descriptorsBefore.data(), g_descriptorsBefore.size()) &&
        ReadExact(kModelInfoPtrs, g_modelPointersBefore.data(), sizeof(g_modelPointersBefore));
}

struct NativeCallResult { void* pointer; DWORD exception; };

NativeCallResult InvokeNative(uint32_t modelId) noexcept {
    NativeCallResult result{};
    __try {
        using Fn = void* (__cdecl*)(int);
        result.pointer = reinterpret_cast<Fn>(kAddVehicleModel)(static_cast<int>(modelId));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result.exception = GetExceptionCode();
    }
    return result;
}

bool PointerTableOnly612And613Changed(uintptr_t expected612, uintptr_t expected613) {
    const auto* after = reinterpret_cast<const uintptr_t*>(kModelInfoPtrs);
    for (uint32_t i = 0; i < kModelCapacity; ++i) {
        uintptr_t expected = g_modelPointersBefore[i];
        if (i == kNewModel) expected = expected612;
        else if (i == kModel613) expected = expected613;
        if (after[i] != expected) return false;
    }
    return true;
}

void LogD0(const D0Snapshot& s, D0Failure failure) {
    Log("OP215_D0 executable=%s", s.executableIdentity ? "PASS" : "FAIL");
    Log("OP215_D0 relocation=%s patch_sites=%s", s.relocationPrepared ? "Prepared" : "FAIL", s.patchSites ? "12/12" : "FAIL");
    Log("OP215_D0 main_thread=%s tid=%lu expected_tid=%lu", s.primaryThread ? "PASS" : "FAIL",
        GetCurrentThreadId(), g_primaryThreadId);
    Log("OP215_D0 samp_r5=%s game_state=%lu", s.sampR5 ? "PASS" : "FAIL",
        *reinterpret_cast<volatile DWORD*>(kGameState));
    Log("OP215_D0 vehicle_count=%lu", static_cast<unsigned long>(s.vehicleCount));
    Log("OP215_D0 constructed=%lu/%lu", static_cast<unsigned long>(s.constructed), static_cast<unsigned long>(kCapacity));
    Log("OP215_D0 model612=%s model613=%s model614=%s", s.model612Null ? "NULL" : "NONNULL",
        s.model613Null ? "NULL" : "NONNULL", s.model614Null ? "NULL" : "NONNULL");
    Log("OP215_D0 descriptor212_unused=%u descriptor213_unused=%u descriptor214_unused=%u",
        s.descriptor212Unused, s.descriptor213Unused, s.descriptor214Unused);
    Log("OP215_D0 model400=%u model560=%u model611=%u originals_intact=%u",
        s.model400Correct, s.model560Correct, s.model611Correct, s.originalsIntact);
    Log("OP215_D0 verdict=%s reason=%s", failure == D0Failure::None ? "PASS" : "FAIL", FailureName(failure));
}

bool RunD0() {
    const D0Snapshot snapshot = Observe();
    const D0Failure failure = ValidateD0Snapshot(snapshot);
    LogD0(snapshot, failure);
    return failure == D0Failure::None;
}

// EXP-001 RunD1, MODEL_613 without TRIPLE: 612 -> descriptor 212, 613 -> 213, 214 untouched.
bool RunD1() {
    const operation213::State state = operation213::GetState();
    if (!CaptureBefore(state)) {
        Log("OP215_D1 preconditions=FAIL reason=snapshot_read");
        return false;
    }
    const uintptr_t descriptor212 = Descriptor(state, 212);
    Log("OP215_D1 main_thread=PASS native_call=0x004C6770 count_before=212");

    const NativeCallResult call = InvokeNative(kNewModel);
    if (call.exception) {
        Log("OP215_D1 native_exception=0x%08lX model=612 verdict=FAIL", call.exception);
        return false;
    }
    const uint32_t countAfter612 = *reinterpret_cast<const uint32_t*>(state.countAddress);
    const auto* models = reinterpret_cast<const uintptr_t*>(kModelInfoPtrs);
    const bool pointerMatches = reinterpret_cast<uintptr_t>(call.pointer) == descriptor212 && models[612] == descriptor212;

    const uintptr_t descriptor213 = Descriptor(state, 213);
    const NativeCallResult call613 = InvokeNative(kModel613);
    if (call613.exception) {
        Log("OP215_D1 native_exception=0x%08lX model=613 verdict=FAIL", call613.exception);
        return false;
    }
    uint32_t validVtables = 0;
    for (uint32_t i = 0; i < kCapacity; ++i)
        if (*reinterpret_cast<const uintptr_t*>(Descriptor(state, i)) == kVehicleVtable) ++validVtables;
    const bool originalsMapped = OriginalsMapped(state);
    const bool originalDescriptorsUnchanged = std::memcmp(
        reinterpret_cast<const void*>(state.descriptorAddress), g_descriptorsBefore.data(), 212 * kDescriptorSize) == 0;
    const uint32_t countAfter = *reinterpret_cast<const uint32_t*>(state.countAddress);
    const bool pointer613Matches = reinterpret_cast<uintptr_t>(call613.pointer) == descriptor213 && models[613] == descriptor213;
    const bool tailDescriptorsUnchanged = std::memcmp(
        reinterpret_cast<const void*>(Descriptor(state, 214)),
        g_descriptorsBefore.data() + 214 * kDescriptorSize, 1 * kDescriptorSize) == 0;
    const bool descriptor214Unused = DescriptorUnused(Descriptor(state, 214));
    const bool pointerTableIsolated = PointerTableOnly612And613Changed(descriptor212, descriptor213);
    const bool noUnrelated = originalDescriptorsUnchanged && tailDescriptorsUnchanged && pointerTableIsolated;
    const bool pass = countAfter612 == 213 && countAfter == 214 && pointerMatches && pointer613Matches &&
        originalsMapped && descriptor214Unused && validVtables == kCapacity && noUnrelated;

    Log("OP215_D1 descriptor_index_612=212 descriptor_index_613=213");
    Log("OP215_D1 count_after_612=%lu count_after_613=%lu", static_cast<unsigned long>(countAfter612),
        static_cast<unsigned long>(countAfter));
    Log("OP215_D1 return_pointer_612=0x%08IX expected=0x%08IX pointer_matches=%u",
        reinterpret_cast<uintptr_t>(call.pointer), descriptor212, pointerMatches);
    Log("OP215_D1 return_pointer_613=0x%08IX expected=0x%08IX pointer_613_matches=%u",
        reinterpret_cast<uintptr_t>(call613.pointer), descriptor213, pointer613Matches);
    Log("OP215_D1 originals_intact=%u original_descriptors_unchanged=%u", originalsMapped, originalDescriptorsUnchanged);
    Log("OP215_D1 descriptor214_unused=%u tail_unchanged=%u", descriptor214Unused, tailDescriptorsUnchanged);
    Log("OP215_D1 vtables=%lu/215 pointer_table_isolated=%u no_unrelated=%u",
        static_cast<unsigned long>(validVtables), pointerTableIsolated, noUnrelated);
    Log("OP215_D1 verdict=%s", pass ? "PASS" : "FAIL");
    return pass;
}

StreamingInfo* Stream(uint32_t id) {
    return reinterpret_cast<StreamingInfo*>(kStreamingInfo + static_cast<uintptr_t>(id) * sizeof(StreamingInfo));
}

bool Readable(const void* pointer, size_t size) {
    if (!pointer || !size) return false;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(pointer, &info, sizeof(info)) || info.State != MEM_COMMIT ||
        info.Protect == PAGE_NOACCESS || (info.Protect & PAGE_GUARD)) return false;
    const uintptr_t begin = reinterpret_cast<uintptr_t>(pointer);
    return begin + size >= begin && begin + size <= reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
}

bool LoadPlayerArchive() {
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return false;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return false;
    wcscpy_s(slash + 1, MAX_PATH - static_cast<size_t>(slash + 1 - path), L"models\\player.img");
    std::array<uint8_t, 32> digest{};
    if (!HashFile(path, digest) || digest != kPlayerImgSha256) return false;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    uint8_t header[8]{};
    DWORD got = 0;
    bool ok = GetFileSizeEx(file, &size) && static_cast<uint64_t>(size.QuadPart) == kPlayerImgSize &&
        ReadFile(file, header, sizeof(header), &got, nullptr) && got == sizeof(header) &&
        std::memcmp(header, "VER2", 4) == 0;
    uint32_t count = 0;
    if (ok) { std::memcpy(&count, header + 4, 4); ok = count > 0 && count < 4096; }
    std::vector<uint8_t> raw(ok ? count * 32u : 0u);
    if (ok) ok = ReadFile(file, raw.data(), static_cast<DWORD>(raw.size()), &got, nullptr) && got == raw.size();
    CloseHandle(file);
    if (!ok) return false;
    g_playerArchiveSectors = static_cast<uint32_t>(kPlayerImgSize / 2048);
    g_playerDirectory.clear();
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t sector = 0;
        uint16_t sectors = 0;
        std::memcpy(&sector, raw.data() + i * 32, 4);
        std::memcpy(&sectors, raw.data() + i * 32 + 4, 2);
        if (!sectors || sector + sectors > g_playerArchiveSectors) return false;
        g_playerDirectory.push_back({sector, sectors});
    }
    return true;
}

// Resolves the live PLAYER.IMG slot and the clothes TXD table from engine memory.
PlayerSlotFailure ResolvePlayerContext(PlayerImageContext& context, std::array<int16_t, kPlayerTxdSlotCount>& txd,
                                       int32_t& index) {
    index = *reinterpret_cast<const int32_t*>(kPlayerImgIndex);
    const char* name = nullptr;
    uint8_t flag = 0xFF;
    if (index >= 0 && static_cast<size_t>(index) < kStreamingFileCount) {
        const auto* file = reinterpret_cast<const uint8_t*>(kStreamingFiles + static_cast<size_t>(index) * kStreamingFileStride);
        name = reinterpret_cast<const char*>(file);
        flag = file[kStreamingFileScanFlag];
    }
    const PlayerSlotFailure failure = ValidatePlayerImageSlot(index, name, kStreamingFileNameSize, flag, kCustomImageId);
    std::memcpy(txd.data(), reinterpret_cast<const void*>(kPlayerTxdSlots), sizeof(int16_t) * txd.size());
    context = {static_cast<uint8_t>(index), g_playerArchiveSectors, txd.data(), txd.size(),
               g_playerDirectory.data(), g_playerDirectory.size()};
    return failure;
}

TxdDef* TxdAt(int index) {
    auto* pool = *reinterpret_cast<TxdPool**>(kTxdPoolPointer);
    if (!pool || !Readable(pool, sizeof(TxdPool)) || !pool->objects || !pool->byteMap ||
        index < 0 || index >= pool->size || pool->size != 20000 ||
        !Readable(pool->byteMap + index, 1) || !Readable(&pool->objects[index], sizeof(TxdDef)) ||
        (pool->byteMap[index] & 0x80)) return nullptr;
    return &pool->objects[index];
}

bool CollisionSane(const uint8_t* collision) {
    if (!Readable(collision, 0x30)) return false;
    const auto* values = reinterpret_cast<const float*>(collision);
    for (int i = 0; i < 10; ++i) if (!std::isfinite(values[i])) return false;
    if (values[0] > values[3] || values[1] > values[4] || values[2] > values[5] || values[9] <= 0.0f) return false;
    const uint8_t flags = collision[0x29];
    return (flags & 1) != 0 && Readable(*reinterpret_cast<void* const*>(collision + 0x2C), 0x10);
}

bool MetadataPreconditions(const operation213::State& state, uint8_t*& descriptor212, uint8_t*& source) {
    descriptor212 = reinterpret_cast<uint8_t*>(Descriptor(state, 212));
    source = reinterpret_cast<uint8_t*>(Descriptor(state, kSourceModel - kOriginalFirstModel));
    using FindTxdFn = int (__cdecl*)(const char*);
    const int existingExp = reinterpret_cast<FindTxdFn>(kFindTxdSlot)(kAssetName);
    const int vehicleTxd = reinterpret_cast<FindTxdFn>(kFindTxdSlot)("vehicle");
    const bool cleanPointers = *reinterpret_cast<void**>(descriptor212 + 0x14) == nullptr &&
        *reinterpret_cast<void**>(descriptor212 + 0x1C) == nullptr &&
        *reinterpret_cast<void**>(descriptor212 + 0x5C) == nullptr;
    const bool sourceReady = *reinterpret_cast<uint32_t*>(source) == kVehicleVtable &&
        *reinterpret_cast<uint16_t*>(source + 0x4A) != 0xFFFF;
    const bool m4NameFree = reinterpret_cast<FindTxdFn>(kFindTxdSlot)(kAsset613Name) == -1;
    return cleanPointers && sourceReady && existingExp == -1 && m4NameFree && vehicleTxd >= 0 &&
        TxdAt(vehicleTxd) != nullptr;
}

// EXP-001 per-model metadata block (identical for 612 and 613).
bool ConfigureModel(uint8_t* descriptor, const uint8_t* source, const char* name, uint32_t model, int handling,
                    int vehicleTxd, int otherTxd, int& txdSlot) {
    using KeyFn = uint32_t (__cdecl*)(const char*);
    using SetTxdFn = void (__thiscall*)(void*, const char*);
    using FindTxdFn = int (__cdecl*)(const char*);
    using ModelLookupFn = void* (__cdecl*)(const char*, int*);
    const bool clean = *reinterpret_cast<void**>(descriptor + 0x14) == nullptr &&
        *reinterpret_cast<void**>(descriptor + 0x1C) == nullptr &&
        *reinterpret_cast<void**>(descriptor + 0x5C) == nullptr;
    if (!clean) { Log("OP215_D2B1 metadata_%lu=FAIL reason=descriptor_not_clean", static_cast<unsigned long>(model)); return false; }
    *reinterpret_cast<uint32_t*>(descriptor + 0x04) = reinterpret_cast<KeyFn>(kKeyGenUppercase)(name);
    std::memcpy(descriptor + 0x32, source + 0x32, 8);
    std::memcpy(descriptor + 0x3C, source + 0x3C, 0x20);
    std::memcpy(descriptor + 0x2B0, source + 0x2B0, 0x26);
    *reinterpret_cast<uint16_t*>(descriptor + 0x4A) = static_cast<uint16_t>(handling);
    reinterpret_cast<SetTxdFn>(kSetTexDictionary)(descriptor, name);

    txdSlot = reinterpret_cast<FindTxdFn>(kFindTxdSlot)(name);
    TxdDef* txd = txdSlot >= 0 ? TxdAt(txdSlot) : nullptr;
    if (txdSlot < 0 || !txd || txdSlot >= kTxdStreamSlots || txdSlot == vehicleTxd || txdSlot == otherTxd ||
        txdSlot == kPlaceholderTxdSlot || *reinterpret_cast<int16_t*>(descriptor + 0x0A) != txdSlot) {
        Log("OP215_D2B1 metadata_%lu=FAIL reason=txd_slot txd=%d", static_cast<unsigned long>(model), txdSlot);
        return false;
    }
    txd->parent = static_cast<int16_t>(vehicleTxd); // exact native IDE-loader side effect at 0x5B701C

    int resolvedId = -1;
    void* resolved = reinterpret_cast<ModelLookupFn>(kGetModelInfo)(name, &resolvedId);
    if (resolved != descriptor || resolvedId != static_cast<int>(model)) {
        Log("OP215_D2B1 metadata_%lu=FAIL reason=lookup lookup_id=%d", static_cast<unsigned long>(model), resolvedId);
        return false;
    }
    Log("OP215_D2B1 model_name=%s lookup_id=%d txd_slot=%d parent_vehicle=%d handling=%d metadata_%lu=PASS",
        name, resolvedId, txdSlot, vehicleTxd, handling, static_cast<unsigned long>(model));
    return true;
}

bool ConfigureMetadata() {
    const operation213::State state = operation213::GetState();
    uint8_t* descriptor212 = nullptr;
    uint8_t* source = nullptr;
    if (!MetadataPreconditions(state, descriptor212, source)) return false;

    using FindTxdFn = int (__cdecl*)(const char*);
    using HandlingFn = int (__thiscall*)(void*, const char*);
    const int handling = reinterpret_cast<HandlingFn>(kGetHandlingId)(
        reinterpret_cast<void*>(kHandlingManager), "SULTAN");
    if (handling < 0 || static_cast<uint16_t>(handling) != *reinterpret_cast<uint16_t*>(source + 0x4A)) return false;
    const int vehicleTxd = reinterpret_cast<FindTxdFn>(kFindTxdSlot)("vehicle");

    if (!ConfigureModel(descriptor212, source, kAssetName, kNewModel, handling, vehicleTxd, -1, g_models[0].txdSlot))
        return false;
    auto* descriptor213 = reinterpret_cast<uint8_t*>(Descriptor(state, 213));
    if (!ConfigureModel(descriptor213, source, kAsset613Name, kModel613, handling, vehicleTxd, g_models[0].txdSlot,
                        g_models[1].txdSlot))
        return false;
    g_vehicleTxdSlot = vehicleTxd;
    g_placeholderSlotBeforeScan = reinterpret_cast<FindTxdFn>(kFindTxdSlot)("NoModelFile");
    Log("OP215_D2B1 asset_profile=%s placeholder_txd=NoModelFile slot_pre_scan=%d pre_scan=1", kAssetProfile,
        g_placeholderSlotBeforeScan);

    // Snapshot after all metadata, so the scan diff contains only scanner-published associations.
    for (uint32_t i = 0; i < kStreamingCount; ++i) {
        const StreamingInfo& s = *Stream(i);
        g_associationsBeforeScan[i] = {s.imageId, s.sector, s.sectors};
    }
    return true;
}

bool VerifyDirectoryMapping() {
    const int expTxd = g_models[0].txdSlot;
    const int m4Txd = g_models[1].txdSlot;
    if (expTxd < 0 || expTxd >= kTxdStreamSlots || m4Txd < 0 || m4Txd >= kTxdStreamSlots) return false;
    const uint32_t txdStreamId = kTxdBase + static_cast<uint32_t>(expTxd);
    const uint32_t m4TxdStreamId = kTxdBase + static_cast<uint32_t>(m4Txd);
    // Manifest of the pinned CUSTOM.IMG (SHA-256 verified before the hook was armed).
    const StreamAssociationEntry manifest[] = {
        {kNewModel, kCustomImageId, kExpectedDffSector, kExpectedDffSectors},
        {kPlaceholderTxdStream, kCustomImageId, kPlaceholderTxdSector, kPlaceholderTxdSectors},
        {txdStreamId, kCustomImageId, kExpectedTxdSector, kExpectedTxdSectors},
        {kModel613, kCustomImageId, kExpected613DffSector, kExpected613DffSectors},
        {m4TxdStreamId, kCustomImageId, kExpected613TxdSector, kExpected613TxdSectors},
    };
    std::array<StreamAssociationEntry, kMaxRecordedChanges> changes{};
    uint32_t changedCount = 0;
    for (uint32_t i = 0; i < kStreamingCount; ++i) {
        const StreamingInfo& after = *Stream(i);
        const auto& before = g_associationsBeforeScan[i];
        if (after.imageId == before.imageId && after.sector == before.sector && after.sectors == before.sectors)
            continue;
        if (changedCount < changes.size()) changes[changedCount] = {i, after.imageId, after.sector, after.sectors};
        ++changedCount;
        Log("OP215_D2B1 association_diff id=%lu before_img=%u before_sector=%lu before_size=%lu "
            "after_img=%u after_sector=%lu after_size=%lu state=%u",
            static_cast<unsigned long>(i), before.imageId, static_cast<unsigned long>(before.sector),
            static_cast<unsigned long>(before.sectors), after.imageId, static_cast<unsigned long>(after.sector),
            static_cast<unsigned long>(after.sectors), after.loadState);
    }
    const AssociationFailure failure = changedCount <= changes.size()
        ? ValidateAssociationChanges(manifest, std::size(manifest), changes.data(), changedCount)
        : AssociationFailure::ExtraChange;

    using FindTxdFn = int (__cdecl*)(const char*);
    const int placeholderSlot = reinterpret_cast<FindTxdFn>(kFindTxdSlot)("NoModelFile");
    const bool placeholderResolved = placeholderSlot == kPlaceholderTxdSlot &&
        kTxdBase + static_cast<uint32_t>(placeholderSlot) == kPlaceholderTxdStream && TxdAt(placeholderSlot) != nullptr;
    const bool notLoaded = Stream(kNewModel)->loadState == 0 && Stream(kPlaceholderTxdStream)->loadState == 0 &&
        Stream(txdStreamId)->loadState == 0 && Stream(kModel613)->loadState == 0 &&
        Stream(m4TxdStreamId)->loadState == 0;
    const uint32_t scanCalls = g_customScanCalls.load();
    const bool onlyExpected = failure == AssociationFailure::None;
    const bool pass = onlyExpected && placeholderResolved && notLoaded && scanCalls == 1;
    Log("OP215_D2B1 allowlist_entries=%u allowlist_failure=%s placeholder_slot=%d placeholder_stream=%lu "
        "placeholder_resolved=%u not_loaded=%u original_scanner_calls=%lu stream_buffer_sectors=%lu",
        static_cast<unsigned>(std::size(manifest)), AssociationFailureName(failure), placeholderSlot,
        static_cast<unsigned long>(kPlaceholderTxdStream), placeholderResolved, notLoaded,
        static_cast<unsigned long>(scanCalls),
        static_cast<unsigned long>(*reinterpret_cast<const uint32_t*>(kStreamingBufferSize)));
    Log("OP215_D2B1 associations_26316_changed=%lu only_expected=%u verdict=%s",
        static_cast<unsigned long>(changedCount), onlyExpected, pass ? "PASS" : "FAIL");
    return pass;
}

bool IsCustomImgPath(const char* path) {
    if (!path) return false;
    constexpr char expected[] = "SAMP\\CUSTOM.IMG";
    constexpr size_t maxLength = 64;
    size_t length = 0;
    while (length < maxLength && path[length] != '\0')
        ++length;
    if (length == maxLength)
        return false;
    // GTA may supply a space-padded IMG path.
    while (length > 0 && path[length - 1] == ' ')
        --length;
    return length == sizeof(expected) - 1 &&
        _strnicmp(path, expected, sizeof(expected) - 1) == 0;
}

void __cdecl HookedLoadCdDirectory(void* image, int imageId) {
    const char* path = static_cast<const char*>(image);
    if (imageId != 0 || g_stage.load() != Stage::Armed || g_customScanCalls.load() != 0) {
        Log("OP215_D2B0 directory_passthrough image_id=%d path=%.64s tid=%lu", imageId,
            path ? path : "<null>", GetCurrentThreadId());
        g_originalLoadCdDirectory(image, imageId);
        return;
    }
    // EXP-001 terminates here; SKR scans natively and stays unmapped instead.
    if (!IsCustomImgPath(path) || GetCurrentThreadId() != g_primaryThreadId) {
        Log("OP215_D2B0 directory_intercept=FAIL image_id=%d path=%.64s tid=%lu -> passthrough, proxy fallback",
            imageId, path ? path : "<null>", GetCurrentThreadId());
        g_stage.store(Stage::Failed);
        g_customScanCalls.fetch_add(1);
        g_originalLoadCdDirectory(image, imageId);
        return;
    }

    Log("OP215_D2B0 directory_intercept=PASS image_id=0 path=SAMP\\CUSTOM.IMG main_thread=PASS");
    const bool prepared = RunD0() && RunD1() && ConfigureMetadata();
    if (!prepared) Log("OP215_D2B1 metadata=FAIL verdict=FAIL (native scan still runs)");
    g_customScanCalls.fetch_add(1);
    g_originalLoadCdDirectory(image, imageId);   // the one and only scan of CUSTOM.IMG
    const bool mapped = prepared && VerifyDirectoryMapping();
    for (size_t i = 0; i < kMappedModels; ++i) g_models[i].resource = mapped ? Resource::Registered : Resource::InArchive;
    g_stage.store(mapped ? Stage::Mapped : Stage::Failed);
    Log("OP215_D2B1 directory_mapped=%u stage=%s", mapped, StageName(g_stage.load()));
}

bool InstallDirectoryHook(bool& written) {
    written = false;
    auto* site = reinterpret_cast<uint8_t*>(kLoadCdDirectory);
    if (std::memcmp(site, kLoadCdDirectoryOriginal.data(), kLoadCdDirectoryOriginal.size()) != 0) return true;
    // Relocated prologue: sub esp,3Ch (3) + mov eax,[esp+40h] (4) -- two whole,
    // position-independent instructions -- then jmp rel32 back to 0x005B6177.
    auto* trampoline = static_cast<uint8_t*>(VirtualAlloc(nullptr, 12, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!trampoline) return true;
    std::memcpy(trampoline, site, 7);
    trampoline[7] = 0xE9;
    *reinterpret_cast<int32_t*>(trampoline + 8) = static_cast<int32_t>(kLoadCdDirectory + 7 -
        (reinterpret_cast<uintptr_t>(trampoline) + 12));
    const uintptr_t trampolineTarget = reinterpret_cast<uintptr_t>(trampoline) + 12 +
        static_cast<uintptr_t>(*reinterpret_cast<int32_t*>(trampoline + 8));
    DWORD trampolineOld = 0;
    const bool trampolineProtected = VirtualProtect(trampoline, 12, PAGE_EXECUTE_READ, &trampolineOld) != FALSE;
    const bool trampolineFlushed = FlushInstructionCache(GetCurrentProcess(), trampoline, 12) != FALSE;
    const bool trampolineValid = trampolineProtected && trampolineFlushed && trampolineTarget == kLoadCdDirectory + 7 &&
        std::memcmp(trampoline, kLoadCdDirectoryOriginal.data(), 7) == 0;
    if (!trampolineValid) {
        Log("OP215_D2B0 trampoline=FAIL protected=%u flushed=%u target=0x%08IX", trampolineProtected,
            trampolineFlushed, trampolineTarget);
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return true;
    }
    g_directoryTrampoline = trampoline;
    g_originalLoadCdDirectory = reinterpret_cast<LoadCdDirectoryFn>(trampoline);

    std::array<uint8_t, 7> patch{0xE9,0,0,0,0,0x90,0x90};
    *reinterpret_cast<int32_t*>(patch.data() + 1) = static_cast<int32_t>(
        reinterpret_cast<uintptr_t>(&HookedLoadCdDirectory) - (kLoadCdDirectory + 5));
    DWORD old = 0;
    if (!VirtualProtect(site, patch.size(), PAGE_EXECUTE_READWRITE, &old)) return true;
    std::memcpy(site, patch.data(), patch.size());
    written = true;
    DWORD ignored = 0;
    const bool siteRestored = VirtualProtect(site, patch.size(), old, &ignored) != FALSE;
    const bool siteFlushed = FlushInstructionCache(GetCurrentProcess(), site, patch.size()) != FALSE;
    const bool readback = std::memcmp(site, patch.data(), patch.size()) == 0;
    const uintptr_t hookTarget = kLoadCdDirectory + 5 + static_cast<uintptr_t>(*reinterpret_cast<int32_t*>(site + 1));
    const bool targetValid = hookTarget == reinterpret_cast<uintptr_t>(&HookedLoadCdDirectory);
    Log("OP215_D2B0 directory_hook=0x005B6170 original=83EC3C8B442440 patched=%u trampoline=%p "
        "trampoline_return=0x%08IX trampoline_protect=RX hook_target_valid=%u site_protect_restored=%u flushed=%u",
        readback, g_directoryTrampoline, trampolineTarget, targetValid, siteRestored, siteFlushed);
    // The site bytes are already written: a later failure means the caller must
    // fail the launch before the primary thread is ever resumed.
    return readback && targetValid && siteRestored && siteFlushed;
}

void CaptureAssociations() {
    for (uint32_t i = 0; i < kStreamingCount; ++i) {
        const StreamingInfo& stream = *Stream(i);
        g_associationsBeforeRequest[i] = {stream.imageId, stream.sector, stream.sectors};
    }
    g_requestBaselineCaptured = true;
}

// D2B3 drift check since the pre-request baseline. EXP-001 required every
// delta to be a classified PLAYER.IMG rebuild (single-player harness). In a
// SA-MP session other native remaps (special characters) are legitimate, so
// here only a delta on a protected ID (612/613, their TXDs, the placeholder)
// is fatal; other unclassified deltas are logged.
bool AssociationsProtectedIntact() {
    PlayerImageContext context{};
    std::array<int16_t, kPlayerTxdSlotCount> txd{};
    int32_t playerIndex = -1;
    const PlayerSlotFailure slot = ResolvePlayerContext(context, txd, playerIndex);
    Log("OP215_D2B3 player_img index=%ld slot=%s txd_slots=%d,%d,%d,%d archive_sectors=%lu directory_entries=%lu",
        static_cast<long>(playerIndex), PlayerSlotFailureName(slot), txd[0], txd[1], txd[2], txd[3],
        static_cast<unsigned long>(g_playerArchiveSectors), static_cast<unsigned long>(g_playerDirectory.size()));
    const uint32_t protectedIds[] = {kNewModel, kPlaceholderTxdStream, kTxdBase + static_cast<uint32_t>(g_models[0].txdSlot),
                                     kModel613, kTxdBase + static_cast<uint32_t>(g_models[1].txdSlot)};
    uint32_t changed = 0, classified = 0, foreign = 0, protectedHits = 0;
    for (uint32_t i = 0; i < kStreamingCount; ++i) {
        const StreamingInfo& stream = *Stream(i);
        const auto& before = g_associationsBeforeRequest[i];
        if (stream.imageId == before.imageId && stream.sector == before.sector && stream.sectors == before.sectors)
            continue;
        ++changed;
        const AssociationDelta delta{i, {before.imageId, before.sector, before.sectors},
                                     {stream.imageId, stream.sector, stream.sectors}};
        bool isProtected = false;
        for (uint32_t id : protectedIds) isProtected = isProtected || id == i;
        const DriftFailure failure = isProtected ? DriftFailure::ProtectedId
            : slot == PlayerSlotFailure::None ? ClassifyDelta(context, delta, protectedIds, std::size(protectedIds))
                                              : DriftFailure::InvalidContext;
        if (failure == DriftFailure::None) ++classified;
        else if (failure == DriftFailure::ProtectedId) ++protectedHits;
        else ++foreign;
        if (changed <= 64)
            Log("OP215_D2B3 association_delta id=%lu before=%u/%lu/%lu after=%u/%lu/%lu state=%u class=%s",
                static_cast<unsigned long>(i), before.imageId, static_cast<unsigned long>(before.sector),
                static_cast<unsigned long>(before.sectors), stream.imageId, static_cast<unsigned long>(stream.sector),
                static_cast<unsigned long>(stream.sectors), stream.loadState,
                failure == DriftFailure::None ? "player_rebuild" : DriftFailureName(failure));
    }
    Log("OP215_D2B3 association_deltas=%lu player_rebuild=%lu unclassified=%lu protected=%lu",
        static_cast<unsigned long>(changed), static_cast<unsigned long>(classified),
        static_cast<unsigned long>(foreign), static_cast<unsigned long>(protectedHits));
    return protectedHits == 0;
}

bool CurrentRegisteredState(const operation213::State& state) {
    const auto* models = reinterpret_cast<uintptr_t*>(kModelInfoPtrs);
    if (*reinterpret_cast<uint32_t*>(state.countAddress) != 214 || models[612] != Descriptor(state, 212) ||
        models[613] != Descriptor(state, 213) || models[614] != 0 || !OriginalsMapped(state)) return false;
    for (uint32_t i = 0; i < kCapacity; ++i)
        if (*reinterpret_cast<uintptr_t*>(Descriptor(state, i)) != kVehicleVtable) return false;
    return true;
}

// Validates one loaded custom model: clump, vehicle structure, owned and distinct
// collision, dedicated TXD with the vehicle parent. Logs the D2B3 asset lines.
// (EXP-001 also required model_refs == 0, true only before any vehicle exists;
// in a session the model may already be referenced, so refs are logged only.)
bool ValidateLoadedModel(const operation213::State& state, uint32_t model, int txdSlot, const char* name,
                         const void* otherCollision) {
    auto* descriptor = reinterpret_cast<uint8_t*>(Descriptor(state, model - kOriginalFirstModel));
    void* clump = *reinterpret_cast<void**>(descriptor + 0x1C);
    void* vehicleStructure = *reinterpret_cast<void**>(descriptor + 0x5C);
    auto* collision = *reinterpret_cast<uint8_t**>(descriptor + 0x14);
    TxdDef* txd = TxdAt(txdSlot);
    const StreamingInfo txdStream = *Stream(kTxdBase + static_cast<uint32_t>(txdSlot));
    const bool clumpReadable = Readable(clump, 8);
    const unsigned clumpType = clumpReadable ? static_cast<unsigned>(*reinterpret_cast<const uint8_t*>(clump)) : 0u;
    const bool clumpValid = clumpReadable && clumpType == 2u &&
        *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(clump) + 4) != nullptr;
    const bool structureValid = Readable(vehicleStructure, 0x314);
    const auto* sourceCollision = *reinterpret_cast<uint8_t* const*>(
        Descriptor(state, kSourceModel - kOriginalFirstModel) + 0x14);
    // Ownership is bit 0x80 of +0x12: CBaseModelInfo::SetColModel (0x4C4BC0) does
    // `or byte [ecx+12h],80h`, and the DFF collision plugin (0x41B2BD) passes owns=1.
    const bool ownsCollision = (descriptor[0x12] & 0x80) != 0;
    const bool collisionDistinct = collision != nullptr && collision != sourceCollision && collision != otherCollision;
    const bool collisionSane = CollisionSane(collision);
    const bool collisionValid = collisionSane && ownsCollision && collisionDistinct;
    float bounds[6]{};
    if (Readable(collision, sizeof(bounds))) std::memcpy(bounds, collision, sizeof(bounds));
    const bool txdValid = txd && txd->dictionary && Readable(txd->dictionary, 4) && txd->refs > 0 &&
        txd->parent == g_vehicleTxdSlot && txdStream.loadState == 1 &&
        *reinterpret_cast<int16_t*>(descriptor + 0x0A) == txdSlot;
    const uint16_t modelRefs = *reinterpret_cast<uint16_t*>(descriptor + 0x08);
    const StreamingInfo modelStream = *Stream(model);
    const uint32_t bufferSectors = *reinterpret_cast<const uint32_t*>(kStreamingBufferSize);
    Log("OP215_D2B3 model_state=LOADED model=%lu clump=%p clump_type=%u clump_valid=%u vehicle_struct=%p vehicle_struct_valid=%u",
        static_cast<unsigned long>(model), clump, clumpType, clumpValid, vehicleStructure, structureValid);
    Log("OP215_D2B3 loaded_asset profile=%s model=%lu name=%s dff_img=%u dff_sector=%lu dff_sectors=%lu "
        "txd_stream=%lu txd_img=%u txd_sector=%lu txd_sectors=%lu stream_buffer_sectors=%lu big_model=%u",
        kAssetProfile, static_cast<unsigned long>(model), name, modelStream.imageId,
        static_cast<unsigned long>(modelStream.sector), static_cast<unsigned long>(modelStream.sectors),
        static_cast<unsigned long>(kTxdBase + static_cast<uint32_t>(txdSlot)), txdStream.imageId,
        static_cast<unsigned long>(txdStream.sector), static_cast<unsigned long>(txdStream.sectors),
        static_cast<unsigned long>(bufferSectors), modelStream.sectors > bufferSectors);
    Log("OP215_D2B3 streaming_memory model=%lu available_kb=%lu used_kb=%lu", static_cast<unsigned long>(model),
        static_cast<unsigned long>(*reinterpret_cast<const uint32_t*>(kStreamingMemoryAvailable) / 1024),
        static_cast<unsigned long>(*reinterpret_cast<const uint32_t*>(kStreamingMemoryUsed) / 1024));
    Log("OP215_D2B3 collision=%p collision_valid=%u sane=%u owns=%u model_flags=0x%02X distinct_from_560=%u "
        "bbox_min=(%.3f,%.3f,%.3f) bbox_max=(%.3f,%.3f,%.3f)",
        collision, collisionValid, collisionSane, ownsCollision, descriptor[0x12], collisionDistinct,
        bounds[0], bounds[1], bounds[2], bounds[3], bounds[4], bounds[5]);
    Log("OP215_D2B3 txd_slot=%d txd_dictionary=%p txd_refs=%u txd_parent=%d expected_parent=%d txd_state=%u txd_valid=%u model_refs=%u",
        txdSlot, txd ? txd->dictionary : nullptr, txd ? txd->refs : 0, txd ? txd->parent : -1,
        g_vehicleTxdSlot, txdStream.loadState, txdValid, modelRefs);
    return clumpValid && structureValid && collisionValid && txdValid;
}

ModelSlot* Find(int modelId) {
    for (auto& slot : g_models) if (static_cast<int>(slot.model) == modelId) return &slot;
    return nullptr;
}

bool InFlight(uint8_t state) { return state != 0 && state != 1; }

} // namespace

const char* StageName(Stage stage) noexcept {
    switch (stage) {
        case Stage::NotStarted: return "NotStarted";
        case Stage::Inactive: return "Inactive";
        case Stage::Armed: return "Armed";
        case Stage::Mapped: return "Mapped";
        case Stage::Failed: return "Failed";
    }
    return "?";
}

const char* ResourceName(Resource resource) noexcept {
    switch (resource) {
        case Resource::Unknown: return "unknown";
        case Resource::Missing: return "missing";
        case Resource::InArchive: return "in_archive";
        case Resource::Registered: return "registered";
        case Resource::Requested: return "requested";
        case Resource::Ready: return "ready";
        case Resource::Rejected: return "rejected";
    }
    return "?";
}

bool PrepareBeforeResume() noexcept {
    Stage expected = Stage::NotStarted;
    if (!g_stage.compare_exchange_strong(expected, Stage::Inactive)) return true;
    Log("OP215_BUILD operation213=ON capacity=%lu asset_profile=%s custom_img=SAMP\\CUSTOM.IMG pid=%lu",
        static_cast<unsigned long>(kCapacity), kAssetProfile, GetCurrentProcessId());

    bool archive = false, playerArchive = false;
    try {
        archive = VerifyCustomArchiveHash();
        playerArchive = LoadPlayerArchive();
    } catch (...) {
        archive = false;
    }
    for (size_t i = 0; i < kMappedModels; ++i) g_models[i].resource = archive ? Resource::InArchive : Resource::Missing;
    Log("OP215_D2B0 archive_sha256=%s player_img_sha256=%s directory_entries=%lu", archive ? "PASS" : "FAIL",
        playerArchive ? "PASS" : "FAIL", static_cast<unsigned long>(g_playerDirectory.size()));
    if (!archive) { Log("OP215_D2B0 custom archive not the certified profile: inactive, proxy fallback"); return true; }

    g_primaryThreadId = operation213::FindSuspendedPrimaryThread();
    if (!g_primaryThreadId) { Log("OP215_D2B0 suspended primary thread not found: inactive"); return true; }
    if (!operation213::Prepare(g_primaryThreadId)) {
        const bool partial = operation213::PartiallyApplied();
        Log("OP215_D2B0 relocation=FAIL status=%lu partial=%u", static_cast<unsigned long>(operation213::GetState().status),
            partial);
        return !partial;
    }
    bool written = false;
    if (!InstallDirectoryHook(written)) return false;
    const bool identity = operation213::VerifyExecutableIdentity();
    Log("OP215_D2B0 executable=%s relocation=Prepared primary_tid=%lu pre_resume_hook=%s", identity ? "PASS" : "FAIL",
        g_primaryThreadId, written ? "PASS" : "FAIL");
    if (!written) return true;   // store relocated, models stay unmapped
    g_stage.store(Stage::Armed);
    return true;
}

Stage GetStage() noexcept { return g_stage.load(); }

Resource GetResource(int modelId) noexcept {
    const ModelSlot* slot = Find(modelId);
    return slot ? slot->resource : Resource::Unknown;
}

bool IsMapped(int modelId) noexcept {
    const Resource r = GetResource(modelId);
    return g_stage.load() == Stage::Mapped &&
        (r == Resource::Registered || r == Resource::Requested || r == Resource::Ready);
}

bool IsReady(int modelId) noexcept {
    return g_stage.load() == Stage::Mapped && GetResource(modelId) == Resource::Ready &&
        Stream(static_cast<uint32_t>(modelId))->loadState == 1;
}

int GetTxdStreamId(int modelId) noexcept {
    const ModelSlot* slot = Find(modelId);
    return slot && slot->txdSlot >= 0 ? static_cast<int>(kTxdBase) + slot->txdSlot : -1;
}

size_t RecheckMappings() noexcept {
    if (g_stage.load() != Stage::Mapped) return 0;
    const StreamAssociationEntry expected[] = {
        {kNewModel, kCustomImageId, kExpectedDffSector, kExpectedDffSectors},
        {kTxdBase + static_cast<uint32_t>(g_models[0].txdSlot), kCustomImageId, kExpectedTxdSector, kExpectedTxdSectors},
        {kModel613, kCustomImageId, kExpected613DffSector, kExpected613DffSectors},
        {kTxdBase + static_cast<uint32_t>(g_models[1].txdSlot), kCustomImageId, kExpected613TxdSector, kExpected613TxdSectors},
    };
    size_t intact = 0;
    for (size_t m = 0; m < kMappedModels; ++m) {
        bool ok = true;
        for (size_t k = m * 2; k < m * 2 + 2; ++k) {
            const StreamingInfo& s = *Stream(expected[k].streamId);
            ok = ok && s.imageId == expected[k].imageId && s.sector == expected[k].sector && s.sectors == expected[k].sectors;
        }
        Log("OP215_RECHECK model=%lu mapping=%s", static_cast<unsigned long>(g_models[m].model), ok ? "intact" : "CHANGED");
        if (ok) ++intact; else g_models[m].resource = Resource::Rejected;
    }
    const operation213::State state = operation213::GetState();
    Log("OP215_RECHECK registered_state=%u vtables_and_count", CurrentRegisteredState(state));
    return intact;
}

void Request(int modelId) noexcept {
    ModelSlot* slot = Find(modelId);
    if (!slot || !IsMapped(modelId)) {
        Log("OP215_D2B2 request model=%d REJECTED resource=%s stage=%s", modelId,
            ResourceName(GetResource(modelId)), StageName(g_stage.load()));
        return;
    }
    if (slot->resource == Resource::Registered && !slot->queued) {
        slot->queued = true;
        Log("OP215_D2B2 request model=%d queued", modelId);
    }
}

// Non-blocking, once per game tick. Both custom DFFs exceed half the streaming
// buffer (native big-model path), so EXP-001 requests them one at a time.
void Tick() noexcept {
    if (g_stage.load() != Stage::Mapped || GetCurrentThreadId() != g_primaryThreadId) return;
    const operation213::State state = operation213::GetState();
    bool busy = false;
    for (size_t i = 0; i < kMappedModels; ++i) {
        ModelSlot& slot = g_models[i];
        const uint8_t loadState = Stream(slot.model)->loadState;
        busy = busy || InFlight(loadState);
        if (slot.resource == Resource::Requested) {
            if (loadState == 1) {
                const void* other = *reinterpret_cast<void* const*>(Descriptor(state, i == 0 ? 213 : 212) + 0x14);
                const bool valid = ValidateLoadedModel(state, slot.model, slot.txdSlot, slot.name, other);
                const bool intact = AssociationsProtectedIntact();
                slot.resource = valid && intact ? Resource::Ready : Resource::Rejected;
                Log("OP215_D2B3 model=%lu load_ms=%llu valid=%u protected_intact=%u verdict=%s",
                    static_cast<unsigned long>(slot.model), GetTickCount64() - slot.requestStarted, valid, intact,
                    valid && intact ? "PASS" : "FAIL");
            } else if (GetTickCount64() - slot.requestStarted > kLoadTimeoutMs) {
                slot.resource = Resource::Rejected;   // left in flight; never re-requested
                Log("OP215_D2B2 load_timeout_ms=20000 model=%lu state=%u verdict=FAIL (proxy fallback)",
                    static_cast<unsigned long>(slot.model), loadState);
            }
        } else if (slot.resource == Resource::Ready && loadState == 0) {
            slot.resource = Resource::Registered;   // streamed out by CStreaming; may be requested again
            Log("OP215_STREAM model=%lu removed by CStreaming -> registered", static_cast<unsigned long>(slot.model));
        }
    }
    if (busy) return;
    for (size_t i = 0; i < kMappedModels; ++i) {
        ModelSlot& slot = g_models[i];
        if (!slot.queued || slot.resource != Resource::Registered) continue;
        if (!g_requestBaselineCaptured) CaptureAssociations();
        slot.queued = false;
        reinterpret_cast<void (__cdecl*)(int, int)>(kRequestModel)(static_cast<int>(slot.model), kRequestFlags);
        slot.requestStarted = GetTickCount64();
        slot.resource = Resource::Requested;
        Log("OP215_D2B2 main_thread=PASS native_call=0x004087E0 model_id=%lu flags=0x%02X request_state=%u",
            static_cast<unsigned long>(slot.model), kRequestFlags, Stream(slot.model)->loadState);
        return;   // one custom model in flight at a time
    }
}

} // namespace skr::operation215
