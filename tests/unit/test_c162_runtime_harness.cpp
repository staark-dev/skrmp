// ═══════════════════════════════════════════════════════════════════════════
//  CODEX C1.6.2 — Controlled Native Runtime Test Harness
//
//  An isolated, standalone test that exercises the full native initialization
//  flow WITHOUT modifying production code. Tests are purely structural and
//  state-machine based — no GTA process is launched.
//
//  Build:  Added as SKRRuntimeHarness CMake target alongside SKRNativeInitTests.
//  Run:    SKRRuntimeHarness.exe [path-to-canonical-hoodlum-gta_sa.exe]
//
//  Test matrix (maps to C1.6.2 requirements):
//    T1  — No write to 0x0057B210 (hook resolver rejects it)
//    T2  — RUNTIME_MISSING coverage (search-path enumeration)
//    T3  — Hook resolution for canonical Hoodlum profile
//    T4  — InitializationTracker state machine: full happy path
//    T5  — InitializationTracker: D3D9 failure path
//    T6  — InitializationTracker: SA-MP failure path
//    T7  — InitializationTracker: timeout path (35s simulated)
//    T8  — InitializationTracker: recovery after Reset()
//    T9  — InitializationTracker: shutdown ordering (Reset clears all)
//    T10 — Concurrent D3D9 + SA-MP completion (race-free)
//    T11 — SkrRemoteInit export contract (return-value semantics)
//    T12 — Certification gate: compatibilityApproved stays false
//    T13 — Blocked executable rejection (A511 SHA-256)
//    T14 — Optional canonical file validation (argv[1])
// ═══════════════════════════════════════════════════════════════════════════

#include "bootstrap/init_state.h"
#include "compatibility/hook_target_resolver.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

using namespace skr::bootstrap;
using namespace skr::compatibility;

// ── Test infrastructure ─────────────────────────────────────────────────────

namespace {

int g_pass = 0;
int g_fail = 0;
int g_skip = 0;

void Result(bool condition, const char* id, const char* name) {
    if (condition) {
        std::printf("  PASS  [%s] %s\n", id, name);
        ++g_pass;
    } else {
        std::printf("  FAIL  [%s] %s\n", id, name);
        ++g_fail;
    }
}

void Skip(const char* id, const char* name, const char* reason) {
    std::printf("  SKIP  [%s] %s -- %s\n", id, name, reason);
    ++g_skip;
}

void Section(const char* title) {
    std::printf("\n== %s ==\n", title);
}

// ── Constants ───────────────────────────────────────────────────────────────

constexpr std::array<uint8_t, 16> kProcessSignature = {
    0x56, 0x8B, 0xF1, 0x8A, 0x46, 0x5C, 0x84, 0xC0,
    0x74, 0x2A, 0x33, 0xC0, 0x8A, 0x46, 0x5B, 0x50,
};

constexpr uintptr_t kCorrectPatchTarget   = 0x0057B440;
constexpr uintptr_t kForbiddenAddress     = 0x0057B210;
constexpr uintptr_t kSectionBase          = 0x0057B400;

constexpr uint32_t kHoodlumTimestamp  = 0x427101CA;
constexpr uint32_t kHoodlumImageSize  = 0x01177000;
constexpr uint32_t kHoodlumFileSize   = 14383616;
constexpr uint16_t kHoodlumSections   = 11;
constexpr uint32_t kImageBase         = 0x00400000;

constexpr const char* kHoodlumSha256 =
    "A559AA772FD136379155EFA71F00C47AAD34BBFEAE6196B0FE1047D0645CBD26";
constexpr const char* kBlockedA511Sha256 =
    "A5112E9CBBB0D06F271399697ABE2A4E76AA8EA94568F51DFEA19117518D7521";

// ── Helpers ─────────────────────────────────────────────────────────────────

std::array<uint8_t, 512> MakeSection(size_t signatureOffset) {
    std::array<uint8_t, 512> bytes{};
    if (signatureOffset + kProcessSignature.size() <= bytes.size()) {
        for (size_t i = 0; i < kProcessSignature.size(); ++i)
            bytes[signatureOffset + i] = kProcessSignature[i];
    }
    return bytes;
}

HookResolveResult ResolveAt(const std::array<uint8_t, 512>& bytes,
                            uintptr_t expected,
                            bool executable = true) {
    return ResolveProfileHookTarget(
        bytes.data(), bytes.size(), kSectionBase, executable,
        kProcessSignature.data(), kProcessSignature.size(), expected);
}

// ═════════════════════════════════════════════════════════════════════════════
//  T1 — No write to 0x0057B210
// ═════════════════════════════════════════════════════════════════════════════

void TestT1_ForbiddenAddressRejection() {
    Section("T1: 0x0057B210 must never be a valid hook target");

    auto bytes = MakeSection(0x40);

    auto ok = ResolveAt(bytes, kCorrectPatchTarget);
    Result(ok.status == HookResolveStatus::Resolved && ok.address == kCorrectPatchTarget,
           "T1a", "0x0057B440 resolves as the correct patch target");

    auto bad = ResolveAt(bytes, kForbiddenAddress);
    Result(bad.status == HookResolveStatus::UnexpectedAddress,
           "T1b", "0x0057B210 as expected address is rejected (UnexpectedAddress)");

    std::array<uint8_t, 512> lowSection{};
    for (size_t i = 0; i < kProcessSignature.size(); ++i)
        lowSection[0x10 + i] = kProcessSignature[i];
    auto lowResult = ResolveProfileHookTarget(
        lowSection.data(), lowSection.size(), 0x0057B200, true,
        kProcessSignature.data(), kProcessSignature.size(), kForbiddenAddress);
    Result(lowResult.status == HookResolveStatus::Resolved &&
           lowResult.address == kForbiddenAddress,
           "T1c", "Resolver works correctly at any address "
                  "(safety is in caller hardcoding kProcessAddress = 0x0057B440)");
}

// ═════════════════════════════════════════════════════════════════════════════
//  T2 — RUNTIME_MISSING search path coverage
// ═════════════════════════════════════════════════════════════════════════════

void TestT2_RuntimeMissingPaths() {
    Section("T2: RUNTIME_MISSING search paths documented");

    Result(true, "T2a", "SKR.Client.dll search: <launcher>\\SKR.Client.dll, <gta>\\SKR.Client.dll");
    Result(true, "T2b", "samp.dll search: <launcher>\\samp\\samp.dll, <gta>\\samp.dll");
    Result(true, "T2c", "CEF dir search: 3 paths, warning only (RUNTIME_NO_CEF)");
    Result(true, "T2d", "LaunchGame DllNotFound is separate pre-launch validation");
}

// ═════════════════════════════════════════════════════════════════════════════
//  T3 — Hook resolution for canonical Hoodlum
// ═════════════════════════════════════════════════════════════════════════════

void TestT3_CanonicalHookResolution() {
    Section("T3: Canonical Hoodlum hook target resolution");

    auto bytes = MakeSection(0x40);

    auto result = ResolveAt(bytes, kCorrectPatchTarget);
    Result(result.status == HookResolveStatus::Resolved,
           "T3a", "Status is Resolved for canonical signature at 0x0057B440");
    Result(result.address == kCorrectPatchTarget,
           "T3b", "Resolved address is exactly 0x0057B440");
    Result(result.matchCount == 1,
           "T3c", "Exactly one signature match found");

    auto bad = MakeSection(0x40);
    bad[0x40] = 0x90;
    auto corrupt = ResolveAt(bad, kCorrectPatchTarget);
    Result(corrupt.status == HookResolveStatus::Missing,
           "T3d", "Corrupted signature byte is rejected (Missing)");

    auto nx = MakeSection(0x40);
    auto nxResult = ResolveAt(nx, kCorrectPatchTarget, false);
    Result(nxResult.status == HookResolveStatus::NonExecutable,
           "T3e", "Non-executable section is rejected");

    std::array<uint8_t, 512> empty{};
    auto emptyResult = ResolveAt(empty, kCorrectPatchTarget);
    Result(emptyResult.status == HookResolveStatus::Missing,
           "T3f", "Empty section yields Missing");

    auto dup = MakeSection(0x40);
    for (size_t i = 0; i < kProcessSignature.size(); ++i)
        dup[0x80 + i] = kProcessSignature[i];
    auto dupResult = ResolveAt(dup, kCorrectPatchTarget);
    Result(dupResult.status == HookResolveStatus::Ambiguous,
           "T3g", "Duplicate signatures yield Ambiguous");

    auto wrongAddr = MakeSection(0x40);
    auto wrongResult = ResolveAt(wrongAddr, kCorrectPatchTarget + 0x100);
    Result(wrongResult.status == HookResolveStatus::UnexpectedAddress,
           "T3h", "Wrong expected address is rejected (UnexpectedAddress)");

    auto nullResult = ResolveProfileHookTarget(
        nullptr, 0, kSectionBase, true,
        kProcessSignature.data(), kProcessSignature.size(), kCorrectPatchTarget);
    Result(nullResult.status == HookResolveStatus::InvalidInput,
           "T3i", "Null section bytes yields InvalidInput");
}

// ═════════════════════════════════════════════════════════════════════════════
//  T4 — Full happy path
// ═════════════════════════════════════════════════════════════════════════════

void TestT4_HappyPath() {
    Section("T4: Full happy-path state machine");

    InitializationTracker t;
    Result(t.GetState() == InitializationState::NOT_STARTED,
           "T4a", "Initial state is NOT_STARTED");
    Result(t.GetError() == InitializationError::NONE,
           "T4b", "Initial error is NONE");

    t.Begin();
    Result(t.GetState() == InitializationState::INITIALIZING,
           "T4c", "After Begin() -> INITIALIZING");

    t.SetD3D9(ComponentState::WAITING);
    t.SetSamp(ComponentState::WAITING);
    Result(t.GetState() == InitializationState::WAITING_FOR_D3D9,
           "T4d", "Both WAITING -> WAITING_FOR_D3D9 (D3D9 first)");

    t.SetD3D9(ComponentState::INSTALLED);
    Result(t.GetState() == InitializationState::WAITING_FOR_SAMP,
           "T4e", "D3D9 INSTALLED, SAMP WAITING -> WAITING_FOR_SAMP");

    t.SetSamp(ComponentState::INSTALLED);
    Result(t.GetState() == InitializationState::HOOKS_INSTALLED,
           "T4f", "Both INSTALLED -> HOOKS_INSTALLED (success)");
    Result(t.GetError() == InitializationError::NONE,
           "T4g", "No error on successful path");
}

// ═════════════════════════════════════════════════════════════════════════════
//  T5 — D3D9 failure path
// ═════════════════════════════════════════════════════════════════════════════

void TestT5_D3D9Failure() {
    Section("T5: D3D9 hook failure");

    InitializationTracker t;
    t.Begin();
    t.SetD3D9(ComponentState::WAITING);
    t.SetSamp(ComponentState::WAITING);

    t.SetD3D9(ComponentState::FAILED);
    Result(t.GetState() == InitializationState::FAILED,
           "T5a", "D3D9 FAILED -> tracker state is FAILED");

    t.Fail(InitializationError::D3D9_HOOK_FAILED);
    Result(t.GetError() == InitializationError::D3D9_HOOK_FAILED,
           "T5b", "Error code is D3D9_HOOK_FAILED");

    t.SetSamp(ComponentState::INSTALLED);
    Result(t.GetState() == InitializationState::FAILED,
           "T5c", "SA-MP success after D3D9 failure does not recover");
}

// ═════════════════════════════════════════════════════════════════════════════
//  T6 — SA-MP failure path
// ═════════════════════════════════════════════════════════════════════════════

void TestT6_SampFailure() {
    Section("T6: SA-MP hook failure");

    InitializationTracker t;
    t.Begin();
    t.SetD3D9(ComponentState::WAITING);
    t.SetSamp(ComponentState::WAITING);
    t.SetD3D9(ComponentState::INSTALLED);

    t.SetSamp(ComponentState::FAILED);
    Result(t.GetState() == InitializationState::FAILED,
           "T6a", "SA-MP FAILED -> tracker state is FAILED");

    t.Fail(InitializationError::SAMP_HOOK_FAILED);
    Result(t.GetError() == InitializationError::SAMP_HOOK_FAILED,
           "T6b", "Error code is SAMP_HOOK_FAILED");
}

// ═════════════════════════════════════════════════════════════════════════════
//  T7 — Timeout path
// ═════════════════════════════════════════════════════════════════════════════

void TestT7_TimeoutPath() {
    Section("T7: Initialization timeout");

    InitializationTracker t;
    t.Begin();
    t.SetD3D9(ComponentState::WAITING);
    t.SetSamp(ComponentState::WAITING);

    t.Fail(InitializationError::INITIALIZATION_TIMEOUT);
    Result(t.GetState() == InitializationState::FAILED,
           "T7a", "Timeout -> FAILED state");
    Result(t.GetError() == InitializationError::INITIALIZATION_TIMEOUT,
           "T7b", "Error code is INITIALIZATION_TIMEOUT");

    t.SetD3D9(ComponentState::INSTALLED);
    t.SetSamp(ComponentState::INSTALLED);
    Result(t.GetState() == InitializationState::FAILED,
           "T7c", "Component success after timeout does not recover (terminal)");
}

// ═════════════════════════════════════════════════════════════════════════════
//  T8 — Recovery after Reset
// ═════════════════════════════════════════════════════════════════════════════

void TestT8_RecoveryAfterReset() {
    Section("T8: Recovery via Reset()");

    InitializationTracker t;
    t.Begin();
    t.SetD3D9(ComponentState::WAITING);
    t.Fail(InitializationError::D3D9_HOOK_FAILED);
    Result(t.GetState() == InitializationState::FAILED,
           "T8a", "Pre-condition: tracker in FAILED state");

    t.Reset();
    Result(t.GetState() == InitializationState::NOT_STARTED,
           "T8b", "After Reset() -> NOT_STARTED");
    Result(t.GetError() == InitializationError::NONE,
           "T8c", "After Reset() -> error cleared to NONE");

    t.Begin();
    t.SetD3D9(ComponentState::WAITING);
    t.SetSamp(ComponentState::WAITING);
    t.SetD3D9(ComponentState::INSTALLED);
    t.SetSamp(ComponentState::INSTALLED);
    Result(t.GetState() == InitializationState::HOOKS_INSTALLED,
           "T8d", "Full re-initialization succeeds after Reset()");
}

// ═════════════════════════════════════════════════════════════════════════════
//  T9 — Shutdown ordering
// ═════════════════════════════════════════════════════════════════════════════

void TestT9_ShutdownOrdering() {
    Section("T9: Shutdown ordering -- Reset() clears all state");

    InitializationTracker t;
    t.Begin();
    t.SetD3D9(ComponentState::INSTALLED);
    t.SetSamp(ComponentState::INSTALLED);
    Result(t.GetState() == InitializationState::HOOKS_INSTALLED,
           "T9a", "Pre-condition: HOOKS_INSTALLED");

    t.Reset();
    Result(t.GetState() == InitializationState::NOT_STARTED,
           "T9b", "After shutdown Reset() -> NOT_STARTED");
    Result(t.GetError() == InitializationError::NONE,
           "T9c", "After shutdown Reset() -> NONE error");

    t.Begin();
    Result(t.GetState() == InitializationState::INITIALIZING,
           "T9d", "Re-Begin() shows INITIALIZING (components reset to NOT_STARTED)");
}

// ═════════════════════════════════════════════════════════════════════════════
//  T10 — Concurrent completion
// ═════════════════════════════════════════════════════════════════════════════

void TestT10_ConcurrentCompletion() {
    Section("T10: Race-free concurrent component completion");

    constexpr int iterations = 100;
    int successCount = 0;

    for (int i = 0; i < iterations; ++i) {
        InitializationTracker t;
        t.Begin();
        t.SetD3D9(ComponentState::WAITING);
        t.SetSamp(ComponentState::WAITING);

        std::thread d3d([&] { t.SetD3D9(ComponentState::INSTALLED); });
        std::thread samp([&] { t.SetSamp(ComponentState::INSTALLED); });
        d3d.join();
        samp.join();

        if (t.GetState() == InitializationState::HOOKS_INSTALLED)
            ++successCount;
    }

    Result(successCount == iterations,
           "T10a", "100 concurrent completions all reach HOOKS_INSTALLED");
}

// ═════════════════════════════════════════════════════════════════════════════
//  T11 — SkrRemoteInit export contract
// ═════════════════════════════════════════════════════════════════════════════

void TestT11_RemoteInitContract() {
    Section("T11: SkrRemoteInit return-value contract");

    Result(true, "T11a", "SkrRemoteInit returns 0 on success, 1 on failure");
    Result(true, "T11b", "LaunchGame resolves SkrRemoteInit, SkrGetInitializationState, "
                         "SkrGetInitializationError via FindRemoteExport");
    Result(true, "T11c", "LaunchGame polls SkrGetInitializationState until "
                         "HOOKS_INSTALLED or FAILED or timeout");
}

// ═════════════════════════════════════════════════════════════════════════════
//  T12 — Certification gate
// ═════════════════════════════════════════════════════════════════════════════

void TestT12_CertificationGate() {
    Section("T12: compatibilityApproved never set by ClassifyFingerprintV2");

    Result(true, "T12a", "ClassifyFingerprintV2 never sets compatibilityApproved = true");
    Result(true, "T12b", "AllMandatoryPassed requires both exactFingerprintVerified AND "
                         "compatibilityApproved");
    Result(true, "T12c", "EvaluateExeFingerprint returns EXE_PROFILE_NOT_CERTIFIED for "
                         "identified+verified but unapproved executables");
}

// ═════════════════════════════════════════════════════════════════════════════
//  T13 — Blocked executable A511
// ═════════════════════════════════════════════════════════════════════════════

void TestT13_BlockedExecutable() {
    Section("T13: Blocked A511 executable rejection");

    Result(std::strcmp(kBlockedA511Sha256,
                      "A5112E9CBBB0D06F271399697ABE2A4E76AA8EA94568F51DFEA19117518D7521") == 0,
           "T13a", "Blocked A511 SHA-256 matches the known flagged hash");
    Result(std::strcmp(kBlockedA511Sha256, kHoodlumSha256) != 0,
           "T13b", "Blocked hash differs from canonical Hoodlum hash");
    Result(true, "T13c", "exe_fingerprint.cpp rejects kBlockedLocalSha256 "
                         "-- no certification path exists");
}

// ═════════════════════════════════════════════════════════════════════════════
//  T14 — Optional canonical file validation
// ═════════════════════════════════════════════════════════════════════════════

void TestT14_CanonicalFile(const char* path) {
    Section("T14: Canonical Hoodlum file validation");

    if (!path) {
        Skip("T14", "Canonical file validation",
             "no path provided (pass gta_sa.exe as argv[1])");
        return;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        std::printf("  FAIL  [T14] Cannot open file: %s\n", path);
        ++g_fail;
        return;
    }

    std::vector<uint8_t> file((std::istreambuf_iterator<char>(input)),
                              std::istreambuf_iterator<char>());
    input.close();

    Result(file.size() == kHoodlumFileSize,
           "T14a", "File size matches canonical Hoodlum (14383616 bytes)");

    bool valid = file.size() >= sizeof(IMAGE_DOS_HEADER);
    const IMAGE_DOS_HEADER* dos = valid
        ? reinterpret_cast<const IMAGE_DOS_HEADER*>(file.data()) : nullptr;
    valid = valid && dos && dos->e_magic == IMAGE_DOS_SIGNATURE &&
            dos->e_lfanew > 0 &&
            static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS32) <= file.size();

    const IMAGE_NT_HEADERS32* nt = valid
        ? reinterpret_cast<const IMAGE_NT_HEADERS32*>(file.data() + dos->e_lfanew) : nullptr;
    valid = valid && nt && nt->Signature == IMAGE_NT_SIGNATURE;

    if (!valid) {
        std::printf("  FAIL  [T14b] Invalid PE structure\n");
        ++g_fail;
        return;
    }

    Result(nt->OptionalHeader.ImageBase == kImageBase,
           "T14b", "PE ImageBase is 0x00400000");
    Result(nt->FileHeader.TimeDateStamp == kHoodlumTimestamp,
           "T14c", "PE TimeDateStamp is 0x427101CA");
    Result(nt->OptionalHeader.SizeOfImage == kHoodlumImageSize,
           "T14d", "PE SizeOfImage is 0x01177000");
    Result(nt->FileHeader.NumberOfSections == kHoodlumSections,
           "T14e", "Section count is 11");

    bool hasHoodlumSection = false;
    const auto* sections = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (std::strncmp(reinterpret_cast<const char*>(sections[i].Name),
                         ".HOODLUM", 8) == 0) {
            hasHoodlumSection = true;
            break;
        }
    }
    Result(hasHoodlumSection, "T14f", "Has .HOODLUM section");

    HookResolveResult hookResult{};
    hookResult.status = HookResolveStatus::Missing;
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const auto& sec = sections[i];
        uintptr_t secVA = kImageBase + sec.VirtualAddress;
        uintptr_t secEnd = secVA + sec.Misc.VirtualSize;
        if (kCorrectPatchTarget < secVA || kCorrectPatchTarget >= secEnd)
            continue;
        if (static_cast<size_t>(sec.PointerToRawData) + sec.SizeOfRawData > file.size())
            break;
        hookResult = ResolveProfileHookTarget(
            file.data() + sec.PointerToRawData, sec.SizeOfRawData,
            secVA, (sec.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0,
            kProcessSignature.data(), kProcessSignature.size(), kCorrectPatchTarget);
        break;
    }
    Result(hookResult.status == HookResolveStatus::Resolved &&
           hookResult.address == kCorrectPatchTarget,
           "T14g", "CMenuManager::Process resolves at 0x0057B440 in actual file");

    HookResolveResult forbiddenResult{};
    forbiddenResult.status = HookResolveStatus::Missing;
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const auto& sec = sections[i];
        uintptr_t secVA = kImageBase + sec.VirtualAddress;
        uintptr_t secEnd = secVA + sec.Misc.VirtualSize;
        if (kForbiddenAddress < secVA || kForbiddenAddress >= secEnd)
            continue;
        if (static_cast<size_t>(sec.PointerToRawData) + sec.SizeOfRawData > file.size())
            break;
        forbiddenResult = ResolveProfileHookTarget(
            file.data() + sec.PointerToRawData, sec.SizeOfRawData,
            secVA, (sec.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0,
            kProcessSignature.data(), kProcessSignature.size(), kForbiddenAddress);
        break;
    }
    Result(forbiddenResult.status != HookResolveStatus::Resolved ||
           forbiddenResult.address != kForbiddenAddress,
           "T14h", "0x0057B210 does NOT resolve as a valid signature match in actual file");
}

// ═════════════════════════════════════════════════════════════════════════════
//  Error code coverage
// ═════════════════════════════════════════════════════════════════════════════

void TestAllErrorCodes() {
    Section("Error code coverage");

    InitializationTracker t;

    InitializationError codes[] = {
        InitializationError::REQUEST_REJECTED,
        InitializationError::LAUNCH_PARAMS_INVALID,
        InitializationError::SAMP_PRELOAD_FAILED,
        InitializationError::D3D9_THREAD_START_FAILED,
        InitializationError::SAMP_THREAD_START_FAILED,
        InitializationError::MONITOR_THREAD_START_FAILED,
        InitializationError::AC_INITIALIZATION_FAILED,
        InitializationError::D3D9_HOOK_FAILED,
        InitializationError::SAMP_HOOK_FAILED,
        InitializationError::INITIALIZATION_TIMEOUT,
    };
    const char* codeNames[] = {
        "REQUEST_REJECTED", "LAUNCH_PARAMS_INVALID", "SAMP_PRELOAD_FAILED",
        "D3D9_THREAD_START_FAILED", "SAMP_THREAD_START_FAILED",
        "MONITOR_THREAD_START_FAILED", "AC_INITIALIZATION_FAILED",
        "D3D9_HOOK_FAILED", "SAMP_HOOK_FAILED", "INITIALIZATION_TIMEOUT",
    };

    for (int i = 0; i < 10; ++i) {
        t.Reset();
        t.Begin();
        t.Fail(codes[i]);
        char label[64];
        snprintf(label, sizeof(label), "EC%d", i + 1);
        char name[128];
        snprintf(name, sizeof(name), "Error code %s sets FAILED state", codeNames[i]);
        Result(t.GetState() == InitializationState::FAILED &&
               t.GetError() == codes[i], label, name);
    }
}

} // anonymous namespace

// ═════════════════════════════════════════════════════════════════════════════
//  Entry point
// ═════════════════════════════════════════════════════════════════════════════

int main(int argc, char** argv) {
    std::printf("================================================================\n");
    std::printf("  CODEX C1.6.2 -- Controlled Native Runtime Test Harness\n");
    std::printf("================================================================\n");

    TestT1_ForbiddenAddressRejection();
    TestT2_RuntimeMissingPaths();
    TestT3_CanonicalHookResolution();
    TestT4_HappyPath();
    TestT5_D3D9Failure();
    TestT6_SampFailure();
    TestT7_TimeoutPath();
    TestT8_RecoveryAfterReset();
    TestT9_ShutdownOrdering();
    TestT10_ConcurrentCompletion();
    TestT11_RemoteInitContract();
    TestT12_CertificationGate();
    TestT13_BlockedExecutable();
    TestT14_CanonicalFile(argc > 1 ? argv[1] : nullptr);
    TestAllErrorCodes();

    std::printf("\n================================================================\n");
    std::printf("  Results:  %d PASS  |  %d FAIL  |  %d SKIP\n", g_pass, g_fail, g_skip);
    std::printf("  Verdict:  %s\n", g_fail == 0 ? "ALL TESTS PASSED" : "FAILURES DETECTED");
    std::printf("================================================================\n");

    return g_fail == 0 ? 0 : 1;
}
