#include "performance/fps_settings.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <string>

namespace skr::performance {
namespace {
std::wstring SettingsPath(bool createFolder) {
    PWSTR local = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)) || !local) return {};
    std::wstring dir(local);
    CoTaskMemFree(local);
    dir += L"\\SKR";
    if (createFolder && !CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        return {};
    return dir + L"\\fps.ini";
}
} // namespace
bool IsValidCap(unsigned cap) {
    return cap == 0 || cap == 30 || cap == 60 || cap == 90 || cap == 120 ||
           cap == 144 || cap == 165 || cap == 240;
}
FpsSettings LoadFpsSettings() {
    FpsSettings value{};
    const auto path = SettingsPath(false);
    if (path.empty()) return value;
    value.showCounter = GetPrivateProfileIntW(L"FPS", L"ShowCounter", 1, path.c_str()) != 0;
    const unsigned parsed = GetPrivateProfileIntW(L"FPS", L"Cap", 0, path.c_str());
    value.cap = IsValidCap(parsed) ? parsed : 0;
    return value;
}
bool SaveFpsSettings(const FpsSettings& value) {
    if (!IsValidCap(value.cap)) return false;
    const auto path = SettingsPath(true);
    if (path.empty()) return false;
    const std::wstring cap = std::to_wstring(value.cap);
    return WritePrivateProfileStringW(L"FPS", L"ShowCounter", value.showCounter ? L"1" : L"0", path.c_str()) &&
           WritePrivateProfileStringW(L"FPS", L"Cap", cap.c_str(), path.c_str());
}
std::string FpsSettingsToJson(const FpsSettings& value) {
    return std::string("{\"showCounter\":") + (value.showCounter ? "true" : "false") +
           ",\"cap\":" + std::to_string(value.cap) + "}";
}
} // namespace skr::performance
