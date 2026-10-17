#pragma once
#include "updater/manifest.h"
#include <cstdint>
#include <string>
#include <vector>
#include <functional>

namespace skr::updater {

enum class UpdateState : uint8_t {
    Idle,
    CheckingManifest,
    Downloading,
    Verifying,
    Staging,
    Swapping,
    Complete,
    RolledBack,
    Failed
};

enum class UpdateError : uint8_t {
    None = 0,
    NetworkError,
    ManifestInvalid,
    ManifestSignatureInvalid,
    HashMismatch,
    DiskFull,
    PathTraversal,
    SwapFailed,
    RollbackFailed,
    VersionTooOld
};

struct UpdateProgress {
    UpdateState state = UpdateState::Idle;
    UpdateError error = UpdateError::None;
    uint32_t totalFiles = 0;
    uint32_t completedFiles = 0;
    uint64_t totalBytes = 0;
    uint64_t downloadedBytes = 0;
    char currentFile[kMaxPathLen]{};
};

// Callback for download requests (production: WinHTTP/WinINet)
using DownloadCallback = std::function<bool(const char* url,
                                            const char* destPath,
                                            uint64_t expectedSize)>;

class UpdateEngine {
public:
    UpdateEngine(const wchar_t* installDir, const wchar_t* stagingDir);

    bool SetManifest(const ReleaseManifest& manifest);

    bool PlanUpdate(const ReleaseManifest& currentManifest);

    bool VerifyStaging();

    bool ApplyUpdate();

    bool Rollback();

    const UpdateProgress& GetProgress() const noexcept { return progress_; }

    void SetDownloader(DownloadCallback cb) { downloader_ = std::move(cb); }

    static bool ComputeFileSha256(const wchar_t* path, uint8_t out[kSha256Size]);

private:
    bool CreateBackup(const ManifestEntry& entry);
    bool SwapFile(const ManifestEntry& entry);
    bool RemoveInstalledFile(const ManifestEntry& entry);
    bool VerifyFile(const wchar_t* path, const ManifestEntry& entry);

    std::wstring MakeInstallPath(const char* relativePath);
    std::wstring MakeStagingPath(const char* relativePath);
    std::wstring MakeBackupPath(const char* relativePath);

    std::wstring installDir_;
    std::wstring stagingDir_;
    std::wstring backupDir_;

    ReleaseManifest targetManifest_;
    std::vector<ManifestEntry> pendingOps_;
    std::vector<std::string> backedUpFiles_;

    UpdateProgress progress_;
    DownloadCallback downloader_;
};

} // namespace skr::updater
