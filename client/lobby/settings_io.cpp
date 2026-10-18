#include "lobby/settings_io.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <cstdio>
#include <cstring>
#include <sstream>

#pragma comment(lib, "shell32.lib")

namespace skr::lobby {

namespace {

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &raw)) && raw) {
        out = raw;
        CoTaskMemFree(raw);
    }
    return out;
}

bool EnsureDir(const std::wstring& dir) {
    if (dir.empty()) return false;
    if (GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    return CreateDirectoryW(dir.c_str(), nullptr) != 0 ||
           GetLastError() == ERROR_ALREADY_EXISTS;
}

std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b-1] == ' ' || s[b-1] == '\t' || s[b-1] == '\r' || s[b-1] == '\n')) --b;
    return s.substr(a, b - a);
}

bool ParseBool(const std::string& v, bool fallback) {
    if (v == "1" || v == "true"  || v == "True"  || v == "TRUE")  return true;
    if (v == "0" || v == "false" || v == "False" || v == "FALSE") return false;
    return fallback;
}

std::string EscapeJson(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c & 0xFF);
                    out += buf;
                } else out += c;
        }
    }
    return out;
}

// Minimal JSON scanner for flat {"k":"v" | number | bool} objects — the
// lobby page only ever sends this shape.
bool FindStringKey(const std::string& j, const std::string& key, std::string& out) {
    std::string pat = "\"" + key + "\"";
    size_t p = j.find(pat);
    if (p == std::string::npos) return false;
    p = j.find(':', p + pat.size());
    if (p == std::string::npos) return false;
    ++p;
    while (p < j.size() && (j[p] == ' ' || j[p] == '\t')) ++p;
    if (p >= j.size() || j[p] != '"') return false;
    ++p;
    std::string val;
    while (p < j.size() && j[p] != '"') {
        if (j[p] == '\\' && p + 1 < j.size()) {
            char n = j[p+1];
            if      (n == 'n') val += '\n';
            else if (n == 't') val += '\t';
            else if (n == 'r') val += '\r';
            else               val += n;
            p += 2;
        } else {
            val += j[p++];
        }
    }
    out = val;
    return true;
}
bool FindRawKey(const std::string& j, const std::string& key, std::string& out) {
    std::string pat = "\"" + key + "\"";
    size_t p = j.find(pat);
    if (p == std::string::npos) return false;
    p = j.find(':', p + pat.size());
    if (p == std::string::npos) return false;
    ++p;
    while (p < j.size() && (j[p] == ' ' || j[p] == '\t')) ++p;
    size_t s = p;
    while (p < j.size() && j[p] != ',' && j[p] != '}' && j[p] != '\n') ++p;
    out = Trim(j.substr(s, p - s));
    return !out.empty();
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}
std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

bool ReadFileUtf8(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (1 << 20)) {
        CloseHandle(h); return false;
    }
    out.resize(static_cast<size_t>(sz.QuadPart));
    DWORD read = 0;
    bool ok = ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &read, nullptr) &&
              read == out.size();
    CloseHandle(h);
    if (!ok) { out.clear(); return false; }
    return true;
}

bool WriteFileUtf8(const std::wstring& path, const std::string& content) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(h, content.data(), static_cast<DWORD>(content.size()),
                        &written, nullptr) && written == content.size();
    CloseHandle(h);
    return ok;
}

} // namespace

std::wstring LauncherIniPath() {
    std::wstring base = KnownFolder(FOLDERID_RoamingAppData);
    if (base.empty()) return {};
    std::wstring dir = base + L"\\SKR";
    EnsureDir(dir);
    return dir + L"\\launcher.ini";
}

std::wstring SampCfgPath() {
    std::wstring docs = KnownFolder(FOLDERID_Documents);
    if (docs.empty()) return {};
    std::wstring dir = docs + L"\\GTA San Andreas User Files\\SAMP";
    EnsureDir(docs + L"\\GTA San Andreas User Files");
    EnsureDir(dir);
    return dir + L"\\sa-mp.cfg";
}

bool LoadSettings(Settings& out) {
    std::wstring path = LauncherIniPath();
    if (path.empty()) return false;
    std::string text;
    if (!ReadFileUtf8(path, text)) return false;

    std::istringstream ss(text);
    std::string line, section;
    while (std::getline(ss, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line.front() == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = Trim(line.substr(0, eq));
        std::string val = Trim(line.substr(eq + 1));
        if (section != "launcher") continue;
        if      (key == "nickname")   out.nickname   = val;
        else if (key == "gtaPath")    out.gtaPath    = val;
        else if (key == "resolution") out.resolution = val;
        else if (key == "windowed")   out.windowed   = ParseBool(val, out.windowed);
        else if (key == "intro")      out.intro      = ParseBool(val, out.intro);
        else if (key == "volume") {
            try { out.volume = std::stoi(val); } catch (...) {}
            if (out.volume < 0) out.volume = 0;
            if (out.volume > 100) out.volume = 100;
        }
    }
    return true;
}

bool SaveSettings(const Settings& in) {
    std::wstring path = LauncherIniPath();
    if (path.empty()) return false;
    std::ostringstream os;
    os << "; SKR Client — launcher settings. Shared with apps/launcher-ui.\r\n"
       << "[launcher]\r\n"
       << "nickname="   << in.nickname   << "\r\n"
       << "gtaPath="    << in.gtaPath    << "\r\n"
       << "resolution=" << in.resolution << "\r\n"
       << "windowed="   << (in.windowed ? "true" : "false") << "\r\n"
       << "intro="      << (in.intro    ? "true" : "false") << "\r\n"
       << "volume="     << in.volume    << "\r\n";
    if (!WriteFileUtf8(path, os.str())) return false;
    MirrorFullscreenToSampCfg(in.windowed);
    return true;
}

bool MirrorFullscreenToSampCfg(bool windowed) {
    std::wstring path = SampCfgPath();
    if (path.empty()) return false;

    std::string text;
    ReadFileUtf8(path, text);  // ok if missing — we will create it

    std::istringstream ss(text);
    std::ostringstream out;
    std::string line;
    bool wrote = false;
    const char* value = windowed ? "0" : "1";
    while (std::getline(ss, line)) {
        std::string t = Trim(line);
        if (t.rfind("fullscreen", 0) == 0) {
            out << "fullscreen " << value << "\r\n";
            wrote = true;
        } else {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            out << line << "\r\n";
        }
    }
    if (!wrote) out << "fullscreen " << value << "\r\n";
    return WriteFileUtf8(path, out.str());
}

std::string SettingsToJson(const Settings& s) {
    std::ostringstream os;
    os << "{"
       << "\"nickname\":\""   << EscapeJson(s.nickname)   << "\","
       << "\"gtaPath\":\""    << EscapeJson(s.gtaPath)    << "\","
       << "\"resolution\":\"" << EscapeJson(s.resolution) << "\","
       << "\"windowed\":"     << (s.windowed ? "true" : "false") << ","
       << "\"intro\":"        << (s.intro    ? "true" : "false") << ","
       << "\"volume\":"       << s.volume
       << "}";
    return os.str();
}

bool SettingsFromJson(const std::string& json, Settings& out) {
    std::string tmp;
    if (FindStringKey(json, "nickname",   tmp)) out.nickname   = tmp;
    if (FindStringKey(json, "gtaPath",    tmp)) out.gtaPath    = tmp;
    if (FindStringKey(json, "resolution", tmp)) out.resolution = tmp;
    if (FindRawKey   (json, "windowed",   tmp)) out.windowed   = ParseBool(tmp, out.windowed);
    if (FindRawKey   (json, "intro",      tmp)) out.intro      = ParseBool(tmp, out.intro);
    if (FindRawKey   (json, "volume",     tmp)) {
        try { out.volume = std::stoi(tmp); } catch (...) {}
        if (out.volume < 0) out.volume = 0;
        if (out.volume > 100) out.volume = 100;
    }
    (void)Utf8ToWide; (void)WideToUtf8;   // helpers available for future use
    return true;
}

} // namespace skr::lobby
