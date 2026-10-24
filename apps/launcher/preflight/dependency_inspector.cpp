#include "dependency_inspector.h"
#include "exe_fingerprint.h"
#include <shlobj.h>

namespace skr::preflight {

// ── Helpers ─────────────────────────────────────────────────────────

static bool FileExists(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static bool DirExists(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static std::wstring FindSystemDll(const wchar_t* dllName) {
    wchar_t sysDir[MAX_PATH]{};
    GetSystemDirectoryW(sysDir, MAX_PATH);
    std::wstring path = std::wstring(sysDir) + L"\\" + dllName;
    if (FileExists(path)) return path;

    wchar_t sysWow[MAX_PATH]{};
    if (GetSystemWow64DirectoryW(sysWow, MAX_PATH) > 0) {
        path = std::wstring(sysWow) + L"\\" + dllName;
        if (FileExists(path)) return path;
    }

    return {};
}

// ── Dependency checks ───────────────────────────────────────────────

std::vector<DependencyStatus> InspectDependencies(const std::wstring& gtaDir) {
    std::vector<DependencyStatus> deps;

    // 1. DirectX 9 — d3dx9_25.dll
    {
        DependencyStatus dep;
        dep.name = "d3dx9_25.dll";
        dep.category = DepCategory::Redistributable;
        dep.foundPath = FindSystemDll(L"d3dx9_25.dll");
        if (dep.foundPath.empty()) {
            std::wstring localPath = gtaDir + L"\\d3dx9_25.dll";
            if (FileExists(localPath)) dep.foundPath = localPath;
        }
        dep.present = !dep.foundPath.empty();
        dep.installSource = "DirectX End-User Runtime (June 2010)";
        if (!dep.present) {
            dep.detail = "GTA San Andreas requires d3dx9_25.dll from the DirectX "
                         "June 2005 SDK redistributable";
        }
        deps.push_back(dep);
    }

    // 2. D3D9 core
    {
        DependencyStatus dep;
        dep.name = "d3d9.dll";
        dep.category = DepCategory::System;
        dep.foundPath = FindSystemDll(L"d3d9.dll");
        dep.present = !dep.foundPath.empty();
        dep.installSource = "Windows built-in";
        deps.push_back(dep);
    }

    // 3. Visual C++ 2015-2022 runtime (x86)
    {
        DependencyStatus dep;
        dep.name = "vcruntime140.dll (x86)";
        dep.category = DepCategory::Redistributable;
        wchar_t sysWow[MAX_PATH]{};
        if (GetSystemWow64DirectoryW(sysWow, MAX_PATH) > 0) {
            std::wstring path = std::wstring(sysWow) + L"\\vcruntime140.dll";
            if (FileExists(path)) dep.foundPath = path;
        }
        if (dep.foundPath.empty()) {
            dep.foundPath = FindSystemDll(L"vcruntime140.dll");
        }
        dep.present = !dep.foundPath.empty();
        dep.installSource = "Microsoft Visual C++ 2015-2022 Redistributable (x86)";
        deps.push_back(dep);
    }

    // 4. MSVCP140.dll (x86)
    {
        DependencyStatus dep;
        dep.name = "msvcp140.dll (x86)";
        dep.category = DepCategory::Redistributable;
        wchar_t sysWow[MAX_PATH]{};
        if (GetSystemWow64DirectoryW(sysWow, MAX_PATH) > 0) {
            std::wstring path = std::wstring(sysWow) + L"\\msvcp140.dll";
            if (FileExists(path)) dep.foundPath = path;
        }
        if (dep.foundPath.empty()) {
            dep.foundPath = FindSystemDll(L"msvcp140.dll");
        }
        dep.present = !dep.foundPath.empty();
        dep.installSource = "Microsoft Visual C++ 2015-2022 Redistributable (x86)";
        deps.push_back(dep);
    }

    // 5. user32.dll (sanity check)
    {
        DependencyStatus dep;
        dep.name = "user32.dll";
        dep.category = DepCategory::System;
        dep.foundPath = FindSystemDll(L"user32.dll");
        dep.present = !dep.foundPath.empty();
        dep.installSource = "Windows built-in";
        deps.push_back(dep);
    }

    // 6. WinHTTP
    {
        DependencyStatus dep;
        dep.name = "winhttp.dll";
        dep.category = DepCategory::System;
        dep.foundPath = FindSystemDll(L"winhttp.dll");
        dep.present = !dep.foundPath.empty();
        dep.installSource = "Windows built-in";
        deps.push_back(dep);
    }

    // 7. WebView2 Runtime
    {
        DependencyStatus dep;
        dep.name = "WebView2Loader.dll";
        dep.category = DepCategory::Optional;
        bool found = false;
        HKEY key = nullptr;
        const wchar_t* regPaths[] = {
            L"SOFTWARE\\WOW6432Node\\Microsoft\\EdgeUpdate\\Clients\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}",
            L"SOFTWARE\\Microsoft\\EdgeUpdate\\Clients\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}",
        };
        for (auto* rp : regPaths) {
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, rp, 0, KEY_READ, &key) == ERROR_SUCCESS) {
                wchar_t ver[64]{};
                DWORD sz = sizeof(ver);
                DWORD type = 0;
                if (RegQueryValueExW(key, L"pv", nullptr, &type,
                                     reinterpret_cast<BYTE*>(ver), &sz) == ERROR_SUCCESS &&
                    type == REG_SZ && ver[0] != L'\0' && ver[0] != L'0') {
                    found = true;
                    dep.detail = "WebView2 Runtime version: ";
                    char narrow[64]{};
                    WideCharToMultiByte(CP_UTF8, 0, ver, -1, narrow, sizeof(narrow), nullptr, nullptr);
                    dep.detail += narrow;
                }
                RegCloseKey(key);
                if (found) break;
            }
        }
        dep.present = found;
        dep.installSource = "Microsoft Edge WebView2 Runtime";
        if (!found) {
            dep.detail = "WebView2 Runtime not detected. The lobby UI requires it.";
        }
        deps.push_back(dep);
    }

    return deps;
}

CheckResult EvaluateDependencies(const std::vector<DependencyStatus>& deps) {
    CheckResult result;
    int missingRequired = 0;
    int missingOptional = 0;
    std::string missingNames;

    for (auto& dep : deps) {
        if (dep.present) continue;
        if (dep.category == DepCategory::Required ||
            dep.category == DepCategory::Redistributable) {
            ++missingRequired;
            if (!missingNames.empty()) missingNames += ", ";
            missingNames += dep.name;
        } else if (dep.category == DepCategory::Optional) {
            ++missingOptional;
        }
    }

    if (missingRequired > 0) {
        result.status = CheckStatus::ActionRequired;
        result.code = "DEPS_MISSING";
        result.summary = "Missing required runtime dependencies";
        result.detail = "Missing: " + missingNames;
        result.remediation = "Install the required redistributables and try again.";
    } else if (missingOptional > 0) {
        result.status = CheckStatus::Warning;
        result.code = "DEPS_OPTIONAL_MISSING";
        result.summary = "Optional dependencies missing (non-blocking)";
        result.detail = std::to_string(missingOptional) + " optional component(s) not found";
        result.remediation = "Some features may not work. Install WebView2 Runtime "
                             "for the full lobby experience.";
    } else {
        result.status = CheckStatus::Verified;
        result.code = "DEPS_OK";
        result.summary = "All runtime dependencies present";
    }

    return result;
}

// ── Bundled runtime check ───────────────────────────────────────────

CheckResult EvaluateBundledRuntime(const std::wstring& launcherDir,
                                    const std::wstring& gtaDir,
                                    std::wstring& outBundledSampPath,
                                    std::wstring& outSkrClientPath,
                                    std::wstring& outCefDir) {
    CheckResult result;
    outBundledSampPath.clear();
    outSkrClientPath.clear();
    outCefDir.clear();

    std::wstring clientPaths[] = {
        launcherDir + L"\\SKR.Client.dll",
        gtaDir + L"\\SKR.Client.dll",
    };
    for (auto& p : clientPaths) {
        if (FileExists(p)) { outSkrClientPath = p; break; }
    }

    std::wstring sampPaths[] = {
        launcherDir + L"\\samp\\samp.dll",
        gtaDir + L"\\samp.dll",
    };
    for (auto& p : sampPaths) {
        if (FileExists(p)) { outBundledSampPath = p; break; }
    }

    std::wstring cefPaths[] = {
        launcherDir + L"\\SKR\\cef",
        launcherDir + L"\\cef",
        gtaDir + L"\\SKR\\cef",
    };
    for (auto& p : cefPaths) {
        if (DirExists(p)) { outCefDir = p; break; }
    }

    bool hasClient = !outSkrClientPath.empty();
    bool hasSamp = !outBundledSampPath.empty();
    bool hasCef = !outCefDir.empty();

    if (hasClient && hasSamp) {
        result.status = CheckStatus::Verified;
        result.code = "RUNTIME_OK";
        result.summary = "SKR runtime components found";

        std::string detail;
        detail += "SKR.Client.dll: present";
        detail += "\nsamp.dll: present";
        detail += "\nCEF runtime: ";
        detail += hasCef ? "present" : "not found (overlay features unavailable)";
        result.detail = detail;

        if (!hasCef) {
            result.status = CheckStatus::Warning;
            result.code = "RUNTIME_NO_CEF";
            result.summary = "CEF runtime not found — overlay features will be unavailable";
        }
    } else {
        result.status = CheckStatus::ActionRequired;
        result.code = "RUNTIME_MISSING";
        result.summary = "Required SKR runtime components missing";

        std::string detail;
        if (!hasClient) detail += "SKR.Client.dll: MISSING\n";
        if (!hasSamp) detail += "samp.dll: MISSING\n";
        result.detail = detail;
        result.remediation = "Reinstall or rebuild the SKR Enhanced client package.";
    }

    return result;
}

} // namespace skr::preflight
