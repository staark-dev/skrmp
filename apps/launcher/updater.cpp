#define WIN32_LEAN_AND_MEAN
#include "updater.h"
#include <windows.h>
#include <bcrypt.h>
#include <cstring>
#include <cstdio>

#pragma comment(lib, "bcrypt.lib")

namespace skr::updater {

UpdateEngine::UpdateEngine(const wchar_t* installDir, const wchar_t* stagingDir)
    : installDir_(installDir), stagingDir_(stagingDir) {
    backupDir_ = installDir_;
    backupDir_ += L"\\.skr-backup";
}

std::wstring UpdateEngine::MakeInstallPath(const char* relativePath) {
    std::wstring result = installDir_ + L"\\";
    while (*relativePath) {
        if (*relativePath == '/')
            result += L'\\';
        else
            result += static_cast<wchar_t>(*relativePath);
        ++relativePath;
    }
    return result;
}

std::wstring UpdateEngine::MakeStagingPath(const char* relativePath) {
    std::wstring result = stagingDir_ + L"\\";
    while (*relativePath) {
        if (*relativePath == '/')
            result += L'\\';
        else
            result += static_cast<wchar_t>(*relativePath);
        ++relativePath;
    }
    return result;
}

std::wstring UpdateEngine::MakeBackupPath(const char* relativePath) {
    std::wstring result = backupDir_ + L"\\";
    while (*relativePath) {
        if (*relativePath == '/')
            result += L'\\';
        else
            result += static_cast<wchar_t>(*relativePath);
        ++relativePath;
    }
    return result;
}

bool UpdateEngine::SetManifest(const ReleaseManifest& manifest) {
    for (auto& entry : manifest.entries) {
        if (!ManifestSerializer::ValidateEntry(entry)) {
            progress_.state = UpdateState::Failed;
            progress_.error = UpdateError::ManifestInvalid;
            return false;
        }
    }
    targetManifest_ = manifest;
    return true;
}

bool UpdateEngine::PlanUpdate(const ReleaseManifest& currentManifest) {
    pendingOps_.clear();
    progress_.state = UpdateState::CheckingManifest;

    for (auto& target : targetManifest_.entries) {
        bool found = false;
        for (auto& current : currentManifest.entries) {
            if (strcmp(target.relativePath, current.relativePath) == 0) {
                found = true;
                if (memcmp(target.sha256, current.sha256, kSha256Size) != 0) {
                    ManifestEntry op = target;
                    op.action = FileAction::Update;
                    pendingOps_.push_back(op);
                }
                break;
            }
        }
        if (!found && target.action != FileAction::Delete) {
            ManifestEntry op = target;
            op.action = FileAction::Add;
            pendingOps_.push_back(op);
        }
    }

    // Files in current but not in target → delete
    for (auto& current : currentManifest.entries) {
        bool found = false;
        for (auto& target : targetManifest_.entries) {
            if (strcmp(target.relativePath, current.relativePath) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            ManifestEntry op = current;
            op.action = FileAction::Delete;
            pendingOps_.push_back(op);
        }
    }

    progress_.totalFiles = static_cast<uint32_t>(pendingOps_.size());
    progress_.completedFiles = 0;
    progress_.totalBytes = 0;
    for (auto& op : pendingOps_) {
        if (op.action != FileAction::Delete)
            progress_.totalBytes += op.sizeBytes;
    }

    return true;
}

bool UpdateEngine::ComputeFileSha256(const wchar_t* path, uint8_t out[kSha256Size]) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = false;

    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0) {
        if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
            uint8_t buf[8192];
            DWORD bytesRead = 0;
            ok = true;
            while (ReadFile(file, buf, sizeof(buf), &bytesRead, nullptr) && bytesRead > 0) {
                if (BCryptHashData(hash, buf, bytesRead, 0) != 0) {
                    ok = false;
                    break;
                }
            }
            if (ok) {
                ok = (BCryptFinishHash(hash, out, kSha256Size, 0) == 0);
            }
            BCryptDestroyHash(hash);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }

    CloseHandle(file);
    return ok;
}

bool UpdateEngine::VerifyFile(const wchar_t* path, const ManifestEntry& entry) {
    uint8_t computed[kSha256Size]{};
    if (!ComputeFileSha256(path, computed)) return false;
    return memcmp(computed, entry.sha256, kSha256Size) == 0;
}

bool UpdateEngine::VerifyStaging() {
    progress_.state = UpdateState::Verifying;
    progress_.completedFiles = 0;

    for (auto& op : pendingOps_) {
        if (op.action == FileAction::Delete) continue;

        auto path = MakeStagingPath(op.relativePath);
        if (!VerifyFile(path.c_str(), op)) {
            progress_.state = UpdateState::Failed;
            progress_.error = UpdateError::HashMismatch;
            strncpy(progress_.currentFile, op.relativePath, kMaxPathLen - 1);
            return false;
        }
        ++progress_.completedFiles;
    }

    return true;
}

static bool EnsureDirectoryExists(const wchar_t* path) {
    // Walk the path creating each component
    std::wstring current;
    const wchar_t* p = path;
    while (*p) {
        current += *p;
        if (*p == L'\\' || *p == L'/') {
            CreateDirectoryW(current.c_str(), nullptr);
        }
        ++p;
    }
    CreateDirectoryW(path, nullptr);
    DWORD attr = GetFileAttributesW(path);
    return (attr != INVALID_FILE_ATTRIBUTES &&
            (attr & FILE_ATTRIBUTE_DIRECTORY));
}

bool UpdateEngine::CreateBackup(const ManifestEntry& entry) {
    auto installPath = MakeInstallPath(entry.relativePath);
    auto backupPath = MakeBackupPath(entry.relativePath);

    DWORD attr = GetFileAttributesW(installPath.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return true; // nothing to back up

    // Ensure backup directory exists
    auto lastSlash = backupPath.rfind(L'\\');
    if (lastSlash != std::wstring::npos) {
        EnsureDirectoryExists(backupPath.substr(0, lastSlash).c_str());
    }

    if (!CopyFileW(installPath.c_str(), backupPath.c_str(), FALSE))
        return false;

    backedUpFiles_.push_back(entry.relativePath);
    return true;
}

bool UpdateEngine::SwapFile(const ManifestEntry& entry) {
    auto stagingPath = MakeStagingPath(entry.relativePath);
    auto installPath = MakeInstallPath(entry.relativePath);

    // Ensure target directory exists
    auto lastSlash = installPath.rfind(L'\\');
    if (lastSlash != std::wstring::npos) {
        EnsureDirectoryExists(installPath.substr(0, lastSlash).c_str());
    }

    return MoveFileExW(stagingPath.c_str(), installPath.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED) != 0;
}

bool UpdateEngine::RemoveInstalledFile(const ManifestEntry& entry) {
    auto installPath = MakeInstallPath(entry.relativePath);
    return ::DeleteFileW(installPath.c_str()) != 0 ||
           GetLastError() == ERROR_FILE_NOT_FOUND;
}

bool UpdateEngine::ApplyUpdate() {
    progress_.state = UpdateState::Staging;
    progress_.completedFiles = 0;
    backedUpFiles_.clear();

    // Phase 1: backup existing files
    for (auto& op : pendingOps_) {
        if (op.action == FileAction::Update || op.action == FileAction::Delete) {
            if (!CreateBackup(op)) {
                progress_.state = UpdateState::Failed;
                progress_.error = UpdateError::SwapFailed;
                strncpy(progress_.currentFile, op.relativePath, kMaxPathLen - 1);
                Rollback();
                return false;
            }
        }
    }

    // Phase 2: swap files
    progress_.state = UpdateState::Swapping;
    for (auto& op : pendingOps_) {
        strncpy(progress_.currentFile, op.relativePath, kMaxPathLen - 1);

        bool ok = false;
        switch (op.action) {
            case FileAction::Add:
            case FileAction::Update:
                ok = SwapFile(op);
                break;
            case FileAction::Delete:
                ok = RemoveInstalledFile(op);
                break;
        }

        if (!ok) {
            progress_.state = UpdateState::Failed;
            progress_.error = UpdateError::SwapFailed;
            Rollback();
            return false;
        }
        ++progress_.completedFiles;
    }

    progress_.state = UpdateState::Complete;
    progress_.error = UpdateError::None;
    progress_.currentFile[0] = '\0';
    return true;
}

bool UpdateEngine::Rollback() {
    bool allOk = true;

    for (auto& relPath : backedUpFiles_) {
        auto backupPath = MakeBackupPath(relPath.c_str());
        auto installPath = MakeInstallPath(relPath.c_str());

        if (!MoveFileExW(backupPath.c_str(), installPath.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
            // Try copy as fallback
            if (!CopyFileW(backupPath.c_str(), installPath.c_str(), FALSE))
                allOk = false;
        }
    }

    if (allOk) {
        progress_.state = UpdateState::RolledBack;
    } else {
        progress_.state = UpdateState::Failed;
        progress_.error = UpdateError::RollbackFailed;
    }

    backedUpFiles_.clear();
    return allOk;
}

} // namespace skr::updater
