#pragma once
// Separate SKR Client FPS preferences; does not change existing lobby settings schema.
#include <string>
namespace skr::performance {
struct FpsSettings { bool showCounter = true; unsigned cap = 0; }; // cap=0: no SKR cap
bool IsValidCap(unsigned cap);
FpsSettings LoadFpsSettings();
bool SaveFpsSettings(const FpsSettings& settings);
std::string FpsSettingsToJson(const FpsSettings& settings);
} // namespace skr::performance
