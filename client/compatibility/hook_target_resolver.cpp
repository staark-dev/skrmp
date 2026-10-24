#include "compatibility/hook_target_resolver.h"

#include <cstring>

namespace skr::compatibility {

HookResolveResult ResolveProfileHookTarget(
    const uint8_t* sectionBytes,
    size_t sectionSize,
    uintptr_t sectionAddress,
    bool sectionExecutable,
    const uint8_t* signature,
    size_t signatureSize,
    uintptr_t expectedAddress) noexcept {
    HookResolveResult result{};
    if (!sectionBytes || !signature || signatureSize == 0 || signatureSize > sectionSize) {
        result.status = HookResolveStatus::InvalidInput;
        return result;
    }
    if (!sectionExecutable) {
        result.status = HookResolveStatus::NonExecutable;
        return result;
    }

    for (size_t offset = 0; offset + signatureSize <= sectionSize; ++offset) {
        if (std::memcmp(sectionBytes + offset, signature, signatureSize) != 0) continue;
        ++result.matchCount;
        result.address = sectionAddress + offset;
    }
    if (result.matchCount == 0) {
        result.status = HookResolveStatus::Missing;
    } else if (result.matchCount != 1) {
        result.status = HookResolveStatus::Ambiguous;
        result.address = 0;
    } else if (result.address != expectedAddress) {
        result.status = HookResolveStatus::UnexpectedAddress;
    } else {
        result.status = HookResolveStatus::Resolved;
    }
    return result;
}

} // namespace skr::compatibility
