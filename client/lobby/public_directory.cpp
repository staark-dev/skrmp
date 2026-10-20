#include "lobby/public_directory.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <initializer_list>

// Source: https://api.open.mp/servers/ — the request made by the open.mp
// server list page (https://open.mp/servers). JSON array of flat objects:
//   ip "host:port", hn hostname, gm gamemode, la language, vn version,
//   pc players, pm max players, pa password (bool), omp, pr (bool).
// Standard WinHTTP TLS validation is kept.

namespace skr::lobby {

namespace {

constexpr wchar_t kHost[] = L"api.open.mp";
constexpr wchar_t kPath[] = L"/servers/";
constexpr size_t kMaxBody = 8 * 1024 * 1024;

bool HttpsGet(std::string& body, std::string& error) {
    body.clear();
    HINTERNET session = WinHttpOpen(L"SKR-Client/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { error = "WinHttpOpen failed"; return false; }
    WinHttpSetTimeouts(session, 5000, 5000, 8000, 8000);
    HINTERNET connect = WinHttpConnect(session, kHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", kPath, nullptr, WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                                : nullptr;
    bool ok = request &&
              WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
              WinHttpReceiveResponse(request, nullptr);
    if (ok) {
        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        if (status != 200) { ok = false; error = "HTTP " + std::to_string(status); }
    } else {
        error = "request failed (" + std::to_string(GetLastError()) + ")";
    }
    while (ok) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request, &avail)) { ok = false; error = "read failed"; break; }
        if (!avail) break;
        if (body.size() + avail > kMaxBody) { ok = false; error = "response too large"; break; }
        const size_t at = body.size();
        body.resize(at + avail);
        DWORD read = 0;
        if (!WinHttpReadData(request, body.data() + at, avail, &read)) { ok = false; error = "read failed"; break; }
        body.resize(at + read);
    }
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return ok;
}

void AppendUtf8(std::string& out, unsigned cp) {
    if (cp < 0x80) out += static_cast<char>(cp);
    else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
    else { out += static_cast<char>(0xF0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
}

// Minimal parser for an array of flat objects (string/number/bool/null values).
class Parser {
public:
    explicit Parser(const std::string& s) : s_(s) {}

    bool Array(std::vector<std::map<std::string, std::string>>& out) {
        Space();
        if (!Eat('[')) return false;
        Space();
        if (Eat(']')) return true;
        for (;;) {
            std::map<std::string, std::string> obj;
            if (!Object(obj)) return false;
            out.push_back(std::move(obj));
            Space();
            if (Eat(']')) return true;
            if (!Eat(',')) return false;
            Space();
        }
    }

private:
    bool Eat(char c) { if (p_ < s_.size() && s_[p_] == c) { ++p_; return true; } return false; }
    void Space() { while (p_ < s_.size() && (s_[p_] == ' ' || s_[p_] == '\n' || s_[p_] == '\r' || s_[p_] == '\t')) ++p_; }

    bool Hex4(unsigned& v) {
        if (p_ + 4 > s_.size()) return false;
        v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = s_[p_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else return false;
        }
        return true;
    }

    bool String(std::string& out) {
        out.clear();
        if (!Eat('"')) return false;
        while (p_ < s_.size()) {
            const char c = s_[p_++];
            if (c == '"') return true;
            if (c != '\\') { out += c; continue; }
            if (p_ >= s_.size()) return false;
            const char e = s_[p_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': case 'f': case 'n': case 'r': case 't': out += ' '; break;
                case 'u': {
                    unsigned cp = 0;
                    if (!Hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF && p_ + 6 <= s_.size() && s_[p_] == '\\' && s_[p_ + 1] == 'u') {
                        p_ += 2;
                        unsigned lo = 0;
                        if (!Hex4(lo)) return false;
                        if (lo >= 0xDC00 && lo <= 0xDFFF) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    AppendUtf8(out, cp);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }

    bool Object(std::map<std::string, std::string>& out) {
        if (!Eat('{')) return false;
        Space();
        if (Eat('}')) return true;
        for (;;) {
            std::string key, value;
            Space();
            if (!String(key)) return false;
            Space();
            if (!Eat(':')) return false;
            Space();
            if (p_ < s_.size() && s_[p_] == '"') {
                if (!String(value)) return false;
            } else {
                const size_t start = p_;
                while (p_ < s_.size() && s_[p_] != ',' && s_[p_] != '}' && s_[p_] != ' ' &&
                       s_[p_] != '\n' && s_[p_] != '\r' && s_[p_] != '\t') ++p_;
                value = s_.substr(start, p_ - start);
                if (value.empty() || value[0] == '{' || value[0] == '[') return false;  // flat only
            }
            out[key] = value;
            Space();
            if (Eat('}')) return true;
            if (!Eat(',')) return false;
        }
    }

    const std::string& s_;
    size_t p_ = 0;
};

void AppendJsonString(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c < 0x20) out += ' ';
        else out += static_cast<char>(c);
    }
    out += '"';
}

std::string Get(const std::map<std::string, std::string>& o, const char* key) {
    auto it = o.find(key);
    return it == o.end() || it->second == "null" ? std::string() : it->second;
}

// This endpoint has a flat JSON schema; aliases permit optional future
// enrichment without assuming access to a server's private config.json.
std::string First(const std::map<std::string, std::string>& o,
                  std::initializer_list<const char*> fields) {
    for (const auto* field : fields) {
        auto value = Get(o, field);
        if (!value.empty()) return value;
    }
    return {};
}

} // namespace

bool FetchOpenMpServers(std::vector<PublicServer>& out, std::string& error) {
    out.clear();
    std::string body;
    if (!HttpsGet(body, error)) return false;
    std::vector<std::map<std::string, std::string>> rows;
    if (!Parser(body).Array(rows)) { error = "unexpected JSON"; return false; }
    for (const auto& r : rows) {
        PublicServer s;
        s.address = Get(r, "ip");
        if (s.address.empty() || s.address.find(':') == std::string::npos) continue;
        s.name = Get(r, "hn");
        s.mode = Get(r, "gm");
        s.language = Get(r, "la");
        s.version = Get(r, "vn");
        s.map = First(r, {"mapname", "map"});
        s.website = First(r, {"weburl", "website"});
        s.discord = First(r, {"discord", "discord_invite", "discord.invite"});
        s.logo = Get(r, "logo");
        s.bannerDark = First(r, {"bannerDark", "banner_dark", "banners.dark"});
        s.bannerLight = First(r, {"bannerLight", "banner_light", "banners.light"});
        s.banner = Get(r, "banner");
        s.players = atoi(Get(r, "pc").c_str());
        s.maxPlayers = atoi(Get(r, "pm").c_str());
        s.password = Get(r, "pa") == "true";
        out.push_back(std::move(s));
    }
    return true;
}

std::string PublicServersToLobbyJson(const std::vector<PublicServer>& list) {
    std::string out = "[";
    for (size_t i = 0; i < list.size(); ++i) {
        const auto& s = list[i];
        if (i) out += ',';
        out += "{\"id\":";        AppendJsonString(out, "omp:" + s.address);
        out += ",\"name\":";      AppendJsonString(out, s.name.empty() ? s.address : s.name);
        out += ",\"mode\":";      AppendJsonString(out, s.mode);
        out += ",\"language\":";  AppendJsonString(out, s.language);
        out += ",\"players\":" + std::to_string(s.players);
        out += ",\"maxPlayers\":" + std::to_string(s.maxPlayers);
        out += ",\"address\":";   AppendJsonString(out, s.address);
        out += ",\"password\":";  out += s.password ? "true" : "false";
        out += ",\"version\":";   AppendJsonString(out, s.version);
        out += ",\"map\":";       AppendJsonString(out, s.map);
        out += ",\"website\":";   AppendJsonString(out, s.website);
        out += ",\"discord\":";   AppendJsonString(out, s.discord);
        out += ",\"logo\":";      AppendJsonString(out, s.logo);
        out += ",\"bannerDark\":"; AppendJsonString(out, s.bannerDark);
        out += ",\"bannerLight\":"; AppendJsonString(out, s.bannerLight);
        out += ",\"banner\":";    AppendJsonString(out, s.banner);
        out += ",\"description\":"; AppendJsonString(out, s.address + (s.version.empty() ? "" : " · " + s.version));
        out += ",\"order\":" + std::to_string(i);
        out += '}';
    }
    out += ']';
    return out;
}

} // namespace skr::lobby
