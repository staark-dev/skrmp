#include "bootstrap/init_state.h"
#include "compatibility/hook_target_resolver.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

using namespace skr::bootstrap;
using namespace skr::compatibility;

namespace {
int failures = 0;

void Expect(bool condition, const char* name) {
    std::printf("%s: %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) ++failures;
}

constexpr std::array<uint8_t, 16> kHoodlumProcess = {
    0x56, 0x8B, 0xF1, 0x8A, 0x46, 0x5C, 0x84, 0xC0,
    0x74, 0x2A, 0x33, 0xC0, 0x8A, 0x46, 0x5B, 0x50,
};
constexpr uintptr_t kSectionBase = 0x0057B400;
constexpr uintptr_t kExpected = 0x0057B440;

std::array<uint8_t, 256> CanonicalSection() {
    std::array<uint8_t, 256> bytes{};
    for (size_t i = 0; i < kHoodlumProcess.size(); ++i) bytes[0x40 + i] = kHoodlumProcess[i];
    return bytes;
}

HookResolveResult Resolve(const std::array<uint8_t, 256>& bytes, bool executable = true,
                          uintptr_t expected = kExpected) {
    return ResolveProfileHookTarget(bytes.data(), bytes.size(), kSectionBase, executable,
                                    kHoodlumProcess.data(), kHoodlumProcess.size(), expected);
}

void TestHookResolution() {
    auto bytes = CanonicalSection();
    auto result = Resolve(bytes);
    Expect(result.status == HookResolveStatus::Resolved && result.address == kExpected,
           "canonical Hoodlum Process target resolves at 0x57B440");

    bytes[0x40] = 0x90;
    Expect(Resolve(bytes).status == HookResolveStatus::Missing,
           "unexpected instruction bytes are rejected");

    bytes = CanonicalSection();
    Expect(Resolve(bytes, false).status == HookResolveStatus::NonExecutable,
           "target in a non-executable section is rejected");

    bytes.fill(0);
    Expect(Resolve(bytes).status == HookResolveStatus::Missing,
           "missing hook target is rejected");

    bytes = CanonicalSection();
    for (size_t i = 0; i < kHoodlumProcess.size(); ++i) bytes[0x80 + i] = kHoodlumProcess[i];
    Expect(Resolve(bytes).status == HookResolveStatus::Ambiguous,
           "ambiguous signature matches are rejected");

    bytes = CanonicalSection();
    Expect(Resolve(bytes, true, kExpected + 0x10).status == HookResolveStatus::UnexpectedAddress,
           "profile-specific target address mismatch is rejected");
}

void TestCanonicalFile(const char* path) {
    std::ifstream input(path, std::ios::binary);
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(input)),
                              std::istreambuf_iterator<char>());
    bool valid = file.size() >= sizeof(IMAGE_DOS_HEADER);
    const IMAGE_DOS_HEADER* dos = valid
        ? reinterpret_cast<const IMAGE_DOS_HEADER*>(file.data()) : nullptr;
    valid = valid && dos->e_magic == IMAGE_DOS_SIGNATURE && dos->e_lfanew > 0 &&
            static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS32) <= file.size();
    const IMAGE_NT_HEADERS32* nt = valid
        ? reinterpret_cast<const IMAGE_NT_HEADERS32*>(file.data() + dos->e_lfanew) : nullptr;
    valid = valid && nt->Signature == IMAGE_NT_SIGNATURE &&
            nt->OptionalHeader.ImageBase == 0x00400000;
    HookResolveResult result{};
    if (valid) {
        const auto* sections = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
            const auto& section = sections[i];
            if (kExpected < nt->OptionalHeader.ImageBase + section.VirtualAddress ||
                kExpected >= nt->OptionalHeader.ImageBase + section.VirtualAddress + section.Misc.VirtualSize)
                continue;
            valid = static_cast<size_t>(section.PointerToRawData) + section.SizeOfRawData <= file.size();
            if (valid) {
                result = ResolveProfileHookTarget(
                    file.data() + section.PointerToRawData, section.SizeOfRawData,
                    nt->OptionalHeader.ImageBase + section.VirtualAddress,
                    (section.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0,
                    kHoodlumProcess.data(), kHoodlumProcess.size(), kExpected);
            }
            break;
        }
    }
    Expect(valid && result.status == HookResolveStatus::Resolved && result.address == kExpected,
           "actual canonical Hoodlum file resolves the verified Process target");
}

void TestInitializationState() {
    InitializationTracker tracker;
    Expect(tracker.GetState() == InitializationState::NOT_STARTED,
           "initial state is NOT_STARTED");
    tracker.Begin();
    Expect(tracker.GetState() == InitializationState::INITIALIZING,
           "accepted request is INITIALIZING, not ready");

    tracker.SetD3D9(ComponentState::WAITING);
    tracker.SetSamp(ComponentState::WAITING);
    Expect(tracker.GetState() == InitializationState::WAITING_FOR_D3D9,
           "delayed D3D9 initialization reports WAITING_FOR_D3D9");
    Expect(tracker.GetState() != InitializationState::HOOKS_INSTALLED,
           "pending asynchronous work cannot report premature success");

    tracker.SetD3D9(ComponentState::INSTALLED);
    Expect(tracker.GetState() == InitializationState::WAITING_FOR_SAMP,
           "installed D3D9 with pending SA-MP reports WAITING_FOR_SAMP");
    tracker.SetSamp(ComponentState::FAILED);
    tracker.Fail(InitializationError::SAMP_HOOK_FAILED);
    Expect(tracker.GetState() == InitializationState::FAILED &&
           tracker.GetError() == InitializationError::SAMP_HOOK_FAILED,
           "SA-MP hook initialization failure is preserved");

    tracker.Reset();
    tracker.Begin();
    tracker.SetD3D9(ComponentState::WAITING);
    tracker.SetSamp(ComponentState::WAITING);
    std::thread d3d([&] { tracker.SetD3D9(ComponentState::INSTALLED); });
    std::thread samp([&] { tracker.SetSamp(ComponentState::INSTALLED); });
    d3d.join();
    samp.join();
    Expect(tracker.GetState() == InitializationState::HOOKS_INSTALLED,
           "asynchronous component completion becomes HOOKS_INSTALLED");

    tracker.Reset();
    tracker.Begin();
    tracker.SetD3D9(ComponentState::WAITING);
    tracker.SetSamp(ComponentState::WAITING);
    tracker.Fail(InitializationError::INITIALIZATION_TIMEOUT);
    Expect(tracker.GetState() == InitializationState::FAILED &&
           tracker.GetError() == InitializationError::INITIALIZATION_TIMEOUT,
           "timeout becomes a bounded unrecoverable failure");
}
} // namespace

int main(int argc, char** argv) {
    TestHookResolution();
    if (argc > 1) TestCanonicalFile(argv[1]);
    TestInitializationState();
    std::printf("Native initialization tests: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
