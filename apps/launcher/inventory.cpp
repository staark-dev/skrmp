#define WIN32_LEAN_AND_MEAN
#include "inventory.h"
#include <windows.h>
#include <cstring>
#include <cstdio>

namespace skr::updater {

FileInventory::FileInventory(const wchar_t* inventoryPath)
    : path_(inventoryPath) {}

// Binary format: u32 count, then count entries of fixed size
static constexpr size_t kEntryDiskSize =
    kMaxPathLen + kSha256Size + 8 + 8 + 1;

bool FileInventory::Load() {
    entries_.clear();

    HANDLE file = CreateFileW(path_.c_str(), GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return true; // no inventory yet

    DWORD bytesRead = 0;
    uint32_t count = 0;
    if (!ReadFile(file, &count, 4, &bytesRead, nullptr) || bytesRead != 4) {
        CloseHandle(file);
        return false;
    }

    if (count > 10000) { CloseHandle(file); return false; }

    entries_.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        auto& e = entries_[i];
        uint8_t buf[kEntryDiskSize];
        if (!ReadFile(file, buf, static_cast<DWORD>(kEntryDiskSize), &bytesRead, nullptr) ||
            bytesRead != kEntryDiskSize) {
            CloseHandle(file);
            entries_.clear();
            return false;
        }

        size_t pos = 0;
        memcpy(e.relativePath, buf + pos, kMaxPathLen); pos += kMaxPathLen;
        memcpy(e.sha256, buf + pos, kSha256Size); pos += kSha256Size;
        memcpy(&e.sizeBytes, buf + pos, 8); pos += 8;
        memcpy(&e.installedAt, buf + pos, 8); pos += 8;
        e.skrOwned = (buf[pos] != 0);
    }

    CloseHandle(file);
    return true;
}

bool FileInventory::Save() {
    HANDLE file = CreateFileW(path_.c_str(), GENERIC_WRITE, 0,
                              nullptr, CREATE_ALWAYS, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    DWORD written = 0;
    uint32_t count = static_cast<uint32_t>(entries_.size());
    WriteFile(file, &count, 4, &written, nullptr);

    for (auto& e : entries_) {
        uint8_t buf[kEntryDiskSize];
        size_t pos = 0;
        memcpy(buf + pos, e.relativePath, kMaxPathLen); pos += kMaxPathLen;
        memcpy(buf + pos, e.sha256, kSha256Size); pos += kSha256Size;
        memcpy(buf + pos, &e.sizeBytes, 8); pos += 8;
        memcpy(buf + pos, &e.installedAt, 8); pos += 8;
        buf[pos] = e.skrOwned ? 1 : 0;

        WriteFile(file, buf, static_cast<DWORD>(kEntryDiskSize), &written, nullptr);
    }

    CloseHandle(file);
    return true;
}

void FileInventory::AddEntry(const ManifestEntry& entry, uint64_t timestamp) {
    // Update existing or append
    for (auto& e : entries_) {
        if (strcmp(e.relativePath, entry.relativePath) == 0) {
            memcpy(e.sha256, entry.sha256, kSha256Size);
            e.sizeBytes = entry.sizeBytes;
            e.installedAt = timestamp;
            e.skrOwned = true;
            return;
        }
    }

    InventoryEntry ie{};
    strncpy(ie.relativePath, entry.relativePath, kMaxPathLen - 1);
    memcpy(ie.sha256, entry.sha256, kSha256Size);
    ie.sizeBytes = entry.sizeBytes;
    ie.installedAt = timestamp;
    ie.skrOwned = true;
    entries_.push_back(ie);
}

void FileInventory::RemoveEntry(const char* relativePath) {
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (strcmp(it->relativePath, relativePath) == 0) {
            entries_.erase(it);
            return;
        }
    }
}

const InventoryEntry* FileInventory::FindEntry(const char* relativePath) const {
    for (auto& e : entries_) {
        if (strcmp(e.relativePath, relativePath) == 0)
            return &e;
    }
    return nullptr;
}

bool FileInventory::DetectConflicts(const ReleaseManifest& manifest,
                                    std::vector<std::string>& conflicts) const {
    conflicts.clear();
    for (auto& entry : manifest.entries) {
        if (entry.action == FileAction::Delete) continue;

        auto existing = FindEntry(entry.relativePath);
        if (existing && !existing->skrOwned) {
            conflicts.push_back(entry.relativePath);
        }
    }
    return conflicts.empty();
}

bool FileInventory::BuildCurrentManifest(ReleaseManifest& out) const {
    out.Clear();
    for (auto& e : entries_) {
        if (!e.skrOwned) continue;
        ManifestEntry me{};
        strncpy(me.relativePath, e.relativePath, kMaxPathLen - 1);
        memcpy(me.sha256, e.sha256, kSha256Size);
        me.sizeBytes = e.sizeBytes;
        me.action = FileAction::Add;
        out.entries.push_back(me);
    }
    return true;
}

} // namespace skr::updater
