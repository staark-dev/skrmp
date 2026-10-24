#pragma once

#include <cstdint>
#include <cwchar>
#include <string>

namespace skr::preflight {

inline bool IsCurrentVerification(uint64_t activeRequestId,
                                  uint64_t completedRequestId,
                                  const std::wstring& activePath,
                                  const std::wstring& completedPath) {
    return activeRequestId == completedRequestId &&
           _wcsicmp(activePath.c_str(), completedPath.c_str()) == 0;
}

} // namespace skr::preflight
