#pragma once
// Operation 213 — relocated CVehicleModelInfo store (ported from SKR-EXP-001,
// certified there for D3.4/D5 dual 612/613). gta_sa 1.0 US only.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstddef>
#include <cstdint>

namespace skr::operation213 {

constexpr uint32_t kOriginalCapacity = 212;
// The 12 patch sites (8 count/base references, 2 store thunks, 2 capacity pushes) are the
// complete set of capacity/base dependencies in gta_sa 1.0 US (D7.1 audit: every absolute
// operand into 0xB1F650..0xB478F8 and every 212/0x308 immediate in CModelInfo code).
// Descriptors 212..214 -> models 612..614.
constexpr uint32_t kRelocatedCapacity = 215;
constexpr size_t kDescriptorSize = 0x308;
constexpr size_t kAllocationSize = sizeof(uint32_t) + kRelocatedCapacity * kDescriptorSize;
static_assert(kAllocationSize == 0x28BBC);

enum class Status : uint32_t {
    NotStarted = 0,
    Preparing,
    Prepared,
    InvalidPrimaryThread,
    PrimaryThreadNotSuspended,
    UnsupportedExecutable,
    UnexpectedOriginalBytes,
    AllocationFailed,
    ProtectionFailed,
    WriteFailed,
    ProtectionRestoreFailed,
    FlushFailed,
    ReadbackMismatch,
};

struct State {
    Status status;
    uintptr_t allocation;
    uintptr_t countAddress;
    uintptr_t descriptorAddress;
    uint32_t allocationSize;
    uint32_t patchedSites;
};

// Finds the GTA primary thread: start address == PE entry point and still
// suspended (launcher creates GTA with CREATE_SUSPENDED). Returns 0 unless
// exactly one such thread exists.
DWORD FindSuspendedPrimaryThread() noexcept;

bool Prepare(DWORD primaryThreadId) noexcept;
State GetState() noexcept;
bool VerifyExecutableIdentity() noexcept;
bool VerifyPatchedSites() noexcept;
// True when a failed Prepare already modified code (process must not run).
bool PartiallyApplied() noexcept;

} // namespace skr::operation213
