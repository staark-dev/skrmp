#include "lobby/settings_io.h"
#include "lobby/flat_json.h"

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

bool FindStringKey(const std::string& json, const std::string& key, std::string& out) {
    std::map<std::string, detail::JsonScalar> values;
    if (!detail::ParseFlatJson(json, values)) return false;
    auto it = values.find(key);
    if (it == values.end() || !it->second.quoted) return false;
    out = it->second.text;
    return true;
}

bool ReadFileUtf8(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 0 || sz.QuadPart > (1 << 20)) {
        CloseHandle(h); return false;
    }
    out.resize(static_cast<size_t>(sz.QuadPart));
    DWORD read = 0;
    bool ok = ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &read, nullptr) &&
              read == out.size();
    CloseHandle(h);
    if (!ok) { out.clear(); return false; }
    if (out.rfind("\xEF\xBB\xBF", 0) == 0) out.erase(0, 3);
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
        // The launcher writes [player]/[game]; older builds of this file wrote [launcher].
        if (section != "launcher" && section != "game" && section != "player") continue;
        if      (key == "nickname")   out.nickname   = val;
        else if (key == "gtaPath" || key == "gta_path") out.gtaPath = val;
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

std::string MergeSettingsIni(const std::string& text, const Settings& in) {
    std::map<std::string, std::string> values = {
        {"nickname", in.nickname}, {"gtaPath", in.gtaPath}, {"gta_path", in.gtaPath},
        {"resolution", in.resolution}, {"windowed", in.windowed ? "true" : "false"},
        {"intro", in.intro ? "true" : "false"}, {"volume", std::to_string(in.volume)}
    };
    std::istringstream input(text);
    std::ostringstream out;
    std::string line, section;
    std::map<std::string, bool> canonical;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string t = Trim(line);
        if (t.size() >= 2 && t.front() == '[' && t.back() == ']') section = t.substr(1, t.size() - 2);
        if (section == "launcher" || section == "player" || section == "game") {
            size_t eq = t.find('=');
            auto it = values.find(Trim(t.substr(0, eq)));
            if (eq != std::string::npos && it != values.end()) {
                line = it->first + "=" + it->second;
                canonical[section + ":" + it->first] = true;
            }
        }
        out << line << "\r\n";
    }
    for (const auto& group : {std::string("player"), std::string("game")}) {
        bool header = false;
        for (const auto& [key, value] : values) {
            if (key == "gtaPath" || (group == "player") != (key == "nickname")) continue;
            if (canonical[group + ":" + key]) continue;
            if (!header) { out << "[" << group << "]\r\n"; header = true; }
            out << key << "=" << value << "\r\n";
        }
    }
    return out.str();
}

bool SaveSettings(const Settings& in) {
    // Never allow a value to inject INI keys or sections.
    for (const auto* value : {&in.nickname, &in.gtaPath, &in.resolution})
        for (unsigned char c : *value) if (c < 32 || c == 127) return false;
    std::wstring path = LauncherIniPath();
    if (path.empty()) return false;
    std::string text;
    if (!ReadFileUtf8(path, text) && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return false;
    std::wstring tmp = path + L".tmp";
    if (!WriteFileUtf8(tmp, MergeSettingsIni(text, in))) return false;
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
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
    std::map<std::string, detail::JsonScalar> values;
    if (!detail::ParseFlatJson(json, values) || values.empty()) return false;
    Settings next = out;
    for (const auto& [key, v] : values) {
        if (key == "resolution" && v.quoted) {
            if (v.text != "auto" && v.text != "1280x720" && v.text != "1366x768" &&
                v.text != "1600x900" && v.text != "1920x1080" && v.text != "2560x1440") return false;
            next.resolution = v.text;
        } else if (key == "windowed" && !v.quoted && (v.text == "true" || v.text == "false")) {
            next.windowed = v.text == "true";
        } else if (key == "volume" && !v.quoted) {
            try { next.volume = std::stoi(v.text); } catch (...) { return false; }
            if (next.volume < 0 || next.volume > 100) return false;
        } else return false;
    }
    out = next;
    return true;
}

bool NicknameFromJson(const std::string& json, std::string& out) {
    std::map<std::string, detail::JsonScalar> values;
    if (!detail::ParseFlatJson(json, values) || values.size() != 1) return false;
    auto it = values.find("nickname");
    if (it == values.end() || !it->second.quoted) return false;
    const auto& name = it->second.text;
    if (name.size() < 3 || name.size() > 24) return false;
    for (char c : name) if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    out = name;
    return true;
}

bool ConnectRequestFromJson(const std::string& json, std::string& address,
                            uint16_t& port, std::string& nickname) {
    std::map<std::string, detail::JsonScalar> values;
    if (!detail::ParseFlatJson(json, values) || values.size() != 2) return false;
    auto addressIt = values.find("address");
    auto nicknameIt = values.find("nickname");
    if (addressIt == values.end() || !addressIt->second.quoted ||
        nicknameIt == values.end() || !nicknameIt->second.quoted) return false;
    std::string canonical;
    if (!NormalizeServerAddress(addressIt->second.text, canonical)) return false;
    std::string nicknameJson = "{\"nickname\":\"" + EscapeJson(nicknameIt->second.text) + "\"}";
    if (!NicknameFromJson(nicknameJson, nickname)) return false;
    const size_t colon = canonical.rfind(':');
    if (colon == std::string::npos) return false;
    address = canonical.substr(0, colon);
    try {
        const unsigned long parsed = std::stoul(canonical.substr(colon + 1));
        if (parsed == 0 || parsed > 65535) return false;
        port = static_cast<uint16_t>(parsed);
    } catch (...) { return false; }
    return true;
}

namespace {

bool IsDnsLabelChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
}

// Drops control chars and INI-breaking newlines, trims, caps the byte length
// without splitting a UTF-8 sequence.
std::string SanitizeServerName(const std::string& in) {
    std::string out;
    for (char c : in) {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) continue;
        out += c;
    }
    out = Trim(out);
    if (out.size() > kMaxServerNameBytes) {
        size_t cut = kMaxServerNameBytes;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;
        out = Trim(out.substr(0, cut));
    }
    return out;
}

} // namespace

bool NormalizeServerAddress(const std::string& in, std::string& out) {
    std::string a = Trim(in);
    size_t colon = a.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= a.size()) return false;
    if (a.find(':') != colon) return false;   // IPv6 / garbage
    std::string host = a.substr(0, colon);
    std::string port = a.substr(colon + 1);

    if (port.size() > 5) return false;
    unsigned long p = 0;
    for (char c : port) {
        if (c < '0' || c > '9') return false;
        p = p * 10 + static_cast<unsigned long>(c - '0');
    }
    if (p < 1 || p > 65535) return false;

    for (char& c : host) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (host.size() > 253) return false;

    // Dotted host: DNS labels of [a-z0-9-], 1..63 chars, no leading/trailing '-'.
    // An all-numeric host must be a valid dotted IPv4 address.
    bool allNumeric = true;
    size_t start = 0;
    int labels = 0;
    while (start <= host.size()) {
        size_t dot = host.find('.', start);
        if (dot == std::string::npos) dot = host.size();
        std::string label = host.substr(start, dot - start);
        if (label.empty() || label.size() > 63) return false;
        if (label.front() == '-' || label.back() == '-') return false;
        for (char c : label) {
            if (!IsDnsLabelChar(c)) return false;
            if (c < '0' || c > '9') allNumeric = false;
        }
        ++labels;
        start = dot + 1;
        if (dot == host.size()) break;
    }
    if (allNumeric) {
        if (labels != 4) return false;
        size_t s0 = 0;
        for (int i = 0; i < 4; ++i) {
            size_t dot = host.find('.', s0);
            std::string octet = host.substr(s0, dot == std::string::npos ? std::string::npos : dot - s0);
            if (octet.size() > 3 || std::stoi(octet) > 255) return false;
            s0 = dot + 1;
        }
    }

    out = host + ":" + std::to_string(p);
    return true;
}

bool LoadPersonalServers(std::vector<PersonalServer>& out) {
    out.clear();
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
        if (section != "servers") continue;
        size_t eq = line.find('=');
        std::string addr = Trim(eq == std::string::npos ? line : line.substr(0, eq));
        PersonalServer ps;
        if (!NormalizeServerAddress(addr, ps.address)) continue;
        if (eq != std::string::npos) ps.name = SanitizeServerName(line.substr(eq + 1));
        bool dup = false;
        for (const auto& e : out) if (e.address == ps.address) { dup = true; break; }
        if (!dup && out.size() < kMaxPersonalServers) out.push_back(ps);
    }
    return true;
}

bool SavePersonalServers(const std::vector<PersonalServer>& in) {
    std::wstring path = LauncherIniPath();
    if (path.empty()) return false;
    std::string text;
    ReadFileUtf8(path, text);   // ok if missing — we will create it

    // Copy every line except the old [servers] section.
    std::istringstream ss(text);
    std::ostringstream os;
    std::string line;
    bool inServers = false;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string t = Trim(line);
        if (!t.empty() && t.front() == '[' && t.back() == ']')
            inServers = (t == "[servers]");
        if (inServers) continue;
        os << line << "\r\n";
    }
    std::string body = os.str();
    while (body.size() >= 4 && body.compare(body.size() - 4, 4, "\r\n\r\n") == 0)
        body.resize(body.size() - 2);
    if (!body.empty()) body += "\r\n";

    body += "[servers]\r\n";
    size_t n = 0;
    for (const auto& ps : in) {
        std::string addr;
        if (!NormalizeServerAddress(ps.address, addr)) continue;
        if (++n > kMaxPersonalServers) break;
        body += addr + "=" + SanitizeServerName(ps.name) + "\r\n";
    }

    std::wstring tmp = path + L".tmp";
    if (!WriteFileUtf8(tmp, body)) return false;
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

std::string PersonalServersToJson(const std::vector<PersonalServer>& list) {
    std::ostringstream os;
    os << "[";
    for (size_t i = 0; i < list.size(); ++i) {
        if (i) os << ",";
        os << "{\"address\":\"" << EscapeJson(list[i].address) << "\","
           << "\"name\":\""    << EscapeJson(list[i].name)    << "\"}";
    }
    os << "]";
    return os.str();
}

bool PersonalServerFromJson(const std::string& json, PersonalServer& out) {
    std::string addr, name;
    if (!FindStringKey(json, "address", addr)) return false;
    if (!NormalizeServerAddress(addr, out.address)) return false;
    FindStringKey(json, "name", name);
    out.name = SanitizeServerName(name);
    return true;
}

} // namespace skr::lobby
