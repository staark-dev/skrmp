#pragma once
#include "updater/manifest.h"
#include <cstdint>
#include <string>
#include <vector>

namespace skr::updater {

struct InventoryEntry {
    char relativePath[kMaxPathLen]{};
    uint8_t sha256[kSha256Size]{};
    uint64_t sizeBytes = 0;
    uint64_t installedAt = 0; // UNIX seconds
    bool skrOwned = true;     // false if pre-existing file we didn't install
};

class FileInventory {
public:
    explicit FileInventory(const wchar_t* inventoryPath);

    bool Load();
    bool Save();

    void AddEntry(const ManifestEntry& entry, uint64_t timestamp);
    void RemoveEntry(const char* relativePath);
    const InventoryEntry* FindEntry(const char* relativePath) const;

    bool DetectConflicts(const ReleaseManifest& manifest,
                         std::vector<std::string>& conflicts) const;

    const std::vector<InventoryEntry>& GetEntries() const { return entries_; }

    bool BuildCurrentManifest(ReleaseManifest& out) const;

private:
    std::wstring path_;
    std::vector<InventoryEntry> entries_;
};

} // namespace skr::updater
