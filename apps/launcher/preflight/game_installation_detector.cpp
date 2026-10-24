#include "game_installation_detector.h"
#include "../../../client/lobby/settings_io.h"
#include <shlobj.h>
#include <fstream>
#include <algorithm>
#include <map>
#include <set>

namespace skr::preflight {

// ── Helpers ─────────────────────────────────────────────────────────

static std::wstring ToLower(std::wstring s) {
    for (auto& c : s) c = towlower(c);
    return s;
}

static bool FileExists(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static std::wstring NormalisePath(const std::wstring& p) {
    wchar_t buf[MAX_PATH]{};
    if (GetFullPathNameW(p.c_str(), MAX_PATH, buf, nullptr))
        return buf;
    return p;
}

static std::wstring ParentDir(const std::wstring& path) {
    auto pos = path.rfind(L'\\');
    if (pos == std::wstring::npos) pos = path.rfind(L'/');
    return (pos != std::wstring::npos) ? path.substr(0, pos) : path;
}

static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                   s.data(), (int)s.size(), nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                        s.data(), (int)s.size(), out.data(), len);
    return out;
}

// ── Resolve a candidate path ────────────────────────────────────────

std::wstring ResolveGtaExePath(const std::wstring& pathOrDir) {
    if (pathOrDir.empty()) return {};
    if (FileExists(pathOrDir)) return NormalisePath(pathOrDir);
    std::wstring withExe = pathOrDir + L"\\gta_sa.exe";
    if (FileExists(withExe)) return NormalisePath(withExe);
    return {};
}

// ── Registry probes ─────────────────────────────────────────────────

static void ProbeRegistry(std::vector<GtaCandidate>& out) {
    struct RegProbe {
        HKEY root;
        const wchar_t* subKey;
        const wchar_t* valueName;
        REGSAM extra;
    };

    static const RegProbe probes[] = {
        { HKEY_LOCAL_MACHINE, L"SOFTWARE\\Rockstar Games\\GTA San Andreas",     L"Location", KEY_WOW64_32KEY },
        { HKEY_LOCAL_MACHINE, L"SOFTWARE\\Rockstar Games\\Grand Theft Auto: San Andreas", L"Location", KEY_WOW64_32KEY },
        { HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Steam App 12120", L"InstallLocation", KEY_WOW64_32KEY },
        { HKEY_CURRENT_USER,  L"SOFTWARE\\SAMP",                                L"gta_sa_exe", 0 },
        { HKEY_LOCAL_MACHINE, L"SOFTWARE\\SAMP",                                L"gta_sa_exe", KEY_WOW64_32KEY },
        { HKEY_LOCAL_MACHINE, L"SOFTWARE\\Rockstar Games\\GTA San Andreas",     L"Location", KEY_WOW64_64KEY },
    };

    for (auto& p : probes) {
        HKEY key = nullptr;
        REGSAM access = KEY_READ | p.extra;
        if (RegOpenKeyExW(p.root, p.subKey, 0, access, &key) != ERROR_SUCCESS)
            continue;

        wchar_t value[MAX_PATH]{};
        DWORD size = sizeof(value) - sizeof(wchar_t);
        DWORD type = 0;
        LSTATUS status = RegQueryValueExW(key, p.valueName, nullptr, &type,
                                           reinterpret_cast<BYTE*>(value), &size);
        RegCloseKey(key);

        if (status != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ))
            continue;

        std::wstring resolved = ResolveGtaExePath(value);
        if (!resolved.empty()) {
            out.push_back({ resolved, ParentDir(resolved), "registry" });
        }
    }
}

// ── Steam libraryfolders.vdf ────────────────────────────────────────

static std::wstring GetSteamDir() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"SOFTWARE\\Valve\\Steam",
                      0, KEY_READ, &key) == ERROR_SUCCESS) {
        wchar_t buf[MAX_PATH]{};
        DWORD sz = sizeof(buf) - sizeof(wchar_t);
        DWORD type = 0;
        if (RegQueryValueExW(key, L"SteamPath", nullptr, &type,
                             reinterpret_cast<BYTE*>(buf), &sz) == ERROR_SUCCESS) {
            RegCloseKey(key);
            return buf;
        }
        RegCloseKey(key);
    }
    return {};
}

static void ProbeSteam(std::vector<GtaCandidate>& out) {
    std::wstring steamDir = GetSteamDir();
    if (steamDir.empty()) return;

    for (auto& c : steamDir) if (c == L'/') c = L'\\';

    std::vector<std::wstring> libraryPaths;
    libraryPaths.push_back(steamDir + L"\\steamapps");

    std::wstring vdfPath = steamDir + L"\\steamapps\\libraryfolders.vdf";
    std::ifstream vdf(vdfPath);
    if (vdf.is_open()) {
        std::string line;
        while (std::getline(vdf, line)) {
            auto pathPos = line.find("\"path\"");
            if (pathPos == std::string::npos) continue;
            auto firstQuote = line.find('"', pathPos + 6);
            if (firstQuote == std::string::npos) continue;
            auto lastQuote = line.find('"', firstQuote + 1);
            if (lastQuote == std::string::npos) continue;
            std::string val = line.substr(firstQuote + 1, lastQuote - firstQuote - 1);
            std::string unescaped;
            for (size_t i = 0; i < val.size(); ++i) {
                if (val[i] == '\\' && i + 1 < val.size() && val[i + 1] == '\\') {
                    unescaped += '\\';
                    ++i;
                } else {
                    unescaped += val[i];
                }
            }
            std::wstring wpath = Utf8ToWide(unescaped);
            if (!wpath.empty())
                libraryPaths.push_back(wpath + L"\\steamapps");
        }
    }

    for (auto& lib : libraryPaths) {
        std::wstring candidate = lib + L"\\common\\Grand Theft Auto San Andreas";
        std::wstring resolved = ResolveGtaExePath(candidate);
        if (!resolved.empty()) {
            out.push_back({ resolved, ParentDir(resolved), "steam" });
        }
    }
}

// ── Well-known paths ────────────────────────────────────────────────

static void ProbeWellKnownPaths(std::vector<GtaCandidate>& out) {
    static const wchar_t* paths[] = {
        L"C:\\Program Files (x86)\\Rockstar Games\\Grand Theft Auto San Andreas",
        L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Grand Theft Auto San Andreas",
        L"C:\\Program Files\\Rockstar Games\\Grand Theft Auto San Andreas",
        L"C:\\Games\\Grand Theft Auto San Andreas",
        L"C:\\GTA San Andreas",
        L"D:\\Games\\Grand Theft Auto San Andreas",
        L"D:\\Program Files (x86)\\Grand Theft Auto San Andreas",
        L"D:\\SteamLibrary\\steamapps\\common\\Grand Theft Auto San Andreas",
        L"E:\\SteamLibrary\\steamapps\\common\\Grand Theft Auto San Andreas",
    };

    for (auto* p : paths) {
        std::wstring resolved = ResolveGtaExePath(p);
        if (!resolved.empty())
            out.push_back({ resolved, ParentDir(resolved), "well-known" });
    }
}

// ── Saved configuration ─────────────────────────────────────────────

static void ProbeConfig(std::vector<GtaCandidate>& out) {
    skr::lobby::Settings settings;
    if (!skr::lobby::LoadSettings(settings) || settings.gtaPath.empty())
        return;

    std::wstring wpath = Utf8ToWide(settings.gtaPath);
    std::wstring resolved = ResolveGtaExePath(wpath);
    if (!resolved.empty())
        out.push_back({ resolved, ParentDir(resolved), "config" });
}

// ── Deduplication ───────────────────────────────────────────────────

void DeduplicateGtaCandidates(std::vector<GtaCandidate>& candidates) {
    struct FileKey {
        DWORD volume = 0;
        DWORD high = 0;
        DWORD low = 0;
        bool operator<(const FileKey& other) const {
            if (volume != other.volume) return volume < other.volume;
            if (high != other.high) return high < other.high;
            return low < other.low;
        }
    };
    std::set<std::wstring> seenPaths;
    std::map<FileKey, size_t> seenFiles;
    std::vector<GtaCandidate> unique;
    unique.reserve(candidates.size());
    auto it = candidates.begin();
    while (it != candidates.end()) {
        const std::wstring pathKey = ToLower(it->exePath);
        if (seenPaths.insert(pathKey).second) {
            HANDLE file = CreateFileW(it->exePath.c_str(), FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            BY_HANDLE_FILE_INFORMATION info{};
            const bool hasIdentity = file != INVALID_HANDLE_VALUE &&
                GetFileInformationByHandle(file, &info) != FALSE;
            if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
            if (hasIdentity) {
                const FileKey key{info.dwVolumeSerialNumber, info.nFileIndexHigh, info.nFileIndexLow};
                const auto duplicate = seenFiles.find(key);
                if (duplicate != seenFiles.end()) {
                    unique[duplicate->second].aliases.push_back(it->exePath);
                    ++it;
                    continue;
                }
                seenFiles.emplace(key, unique.size());
            }
            if (it->aliases.empty()) it->aliases.push_back(it->exePath);
            unique.push_back(std::move(*it));
        }
        ++it;
    }
    candidates = std::move(unique);
}

// ── Public API ──────────────────────────────────────────────────────

std::vector<GtaCandidate> DetectGtaInstallations(const std::wstring& launcherDir) {
    std::vector<GtaCandidate> candidates;

    ProbeConfig(candidates);

    {
        std::wstring resolved = ResolveGtaExePath(launcherDir);
        if (!resolved.empty())
            candidates.push_back({ resolved, ParentDir(resolved), "launcher-dir" });
    }
    {
        std::wstring parent = ParentDir(launcherDir);
        std::wstring resolved = ResolveGtaExePath(parent);
        if (!resolved.empty())
            candidates.push_back({ resolved, ParentDir(resolved), "launcher-parent" });
    }

    ProbeRegistry(candidates);
    ProbeSteam(candidates);
    ProbeWellKnownPaths(candidates);

    DeduplicateGtaCandidates(candidates);
    return candidates;
}

CheckResult EvaluateGtaDiscovery(const std::vector<GtaCandidate>& candidates,
                                  std::wstring& outExePath,
                                  std::wstring& outDir) {
    CheckResult result;

    if (candidates.empty()) {
        result.status = CheckStatus::ActionRequired;
        result.code = "GTA_NOT_FOUND";
        result.summary = "GTA San Andreas installation not found";
        result.detail = "Searched: saved config, launcher directory, registry, "
                        "Steam libraries, and well-known paths";
        result.remediation = "Select your GTA San Andreas installation folder manually";
        return result;
    }

    outExePath = candidates[0].exePath;
    outDir = candidates[0].directory;

    result.status = CheckStatus::Verified;
    result.code = "GTA_FOUND";
    result.summary = "GTA San Andreas found via " + candidates[0].source;

    if (candidates.size() > 1) {
        result.detail = "Found " + std::to_string(candidates.size()) +
                        " installation(s); using " + candidates[0].source + " candidate";
    }

    return result;
}

} // namespace skr::preflight
