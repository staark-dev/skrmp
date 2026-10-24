#pragma once

#include <cstddef>
#include <cstdint>

namespace skr::compatibility {

enum class HookResolveStatus : uint8_t {
    Resolved = 0,
    Missing,
    Ambiguous,
    UnexpectedAddress,
    NonExecutable,
    InvalidInput,
};

struct HookResolveResult {
    HookResolveStatus status = HookResolveStatus::InvalidInput;
    uintptr_t address = 0;
    size_t matchCount = 0;
};

// Resolves an exact signature inside one PE section. The caller must say
// whether that section is executable; data sections are always rejected.
// expectedAddress is profile-specific and prevents a signature from silently
// approving a different executable layout.
HookResolveResult ResolveProfileHookTarget(
    const uint8_t* sectionBytes,
    size_t sectionSize,
    uintptr_t sectionAddress,
    bool sectionExecutable,
    const uint8_t* signature,
    size_t signatureSize,
    uintptr_t expectedAddress) noexcept;

} // namespace skr::compatibility
