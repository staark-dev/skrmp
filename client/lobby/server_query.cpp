#include "lobby/server_query.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdio>
#include <cstring>

// Wire format: SA-MP query mechanism (public, also implemented by open.mp).
// Request: "SAMP" + server IPv4 (4 bytes) + port (uint16 LE) + opcode 'i'.
// Reply:   the same 11-byte header, then
//          uint8 password, uint16 players, uint16 maxPlayers,
//          uint32 len + hostname, uint32 len + gamemode, uint32 len + language.

namespace skr::lobby {

namespace {

constexpr size_t kHeader = 11;

bool SplitAddress(const std::string& address, std::string& host, uint16_t& port) {
    const size_t colon = address.rfind(':');
    if (colon == std::string::npos || colon == 0) return false;
    host = address.substr(0, colon);
    const int p = atoi(address.c_str() + colon + 1);
    if (p <= 0 || p > 65535) return false;
    port = static_cast<uint16_t>(p);
    return true;
}

// Server strings are Windows-1252 (SA-MP convention); the page needs UTF-8.
std::string Cp1252ToUtf8(const char* data, size_t len) {
    if (!len) return {};
    int wlen = MultiByteToWideChar(1252, 0, data, static_cast<int>(len), nullptr, 0);
    if (wlen <= 0) return {};
    std::wstring w(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(1252, 0, data, static_cast<int>(len), w.data(), wlen);
    int ulen = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), wlen, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(ulen > 0 ? ulen : 0), '\0');
    if (ulen > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), wlen, out.data(), ulen, nullptr, nullptr);
    return out;
}

bool ReadString(const char* buf, size_t size, size_t& pos, std::string& out) {
    if (pos + 4 > size) return false;
    uint32_t len = 0;
    memcpy(&len, buf + pos, 4);
    pos += 4;
    if (len > 1024 || pos + len > size) return false;
    out = Cp1252ToUtf8(buf + pos, len);
    pos += len;
    return true;
}

void AppendJsonString(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) { char e[8]; sprintf_s(e, "\\u%04x", c); out += e; }
                else out += static_cast<char>(c);
        }
    }
    out += '"';
}

} // namespace

ServerInfo QueryServer(const std::string& address, unsigned timeoutMs) {
    ServerInfo info;
    info.address = address;

    std::string host;
    uint16_t port = 0;
    if (!SplitAddress(address, host, port)) return info;

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return info;

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* resolved = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &resolved) != 0 || !resolved) {
        WSACleanup();
        return info;
    }
    sockaddr_in to = *reinterpret_cast<sockaddr_in*>(resolved->ai_addr);
    freeaddrinfo(resolved);
    to.sin_port = htons(port);

    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) { WSACleanup(); return info; }

    char packet[kHeader];
    memcpy(packet, "SAMP", 4);
    memcpy(packet + 4, &to.sin_addr.s_addr, 4);
    packet[8] = static_cast<char>(port & 0xFF);
    packet[9] = static_cast<char>(port >> 8);
    packet[10] = 'i';

    LARGE_INTEGER freq{}, start{}, end{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    sendto(s, packet, sizeof(packet), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));

    char buf[2048];
    fd_set set;
    FD_ZERO(&set);
    FD_SET(s, &set);
    timeval tv{static_cast<long>(timeoutMs / 1000), static_cast<long>((timeoutMs % 1000) * 1000)};
    int n = 0;
    if (select(0, &set, nullptr, nullptr, &tv) > 0) {
        sockaddr_in from{};
        int fromLen = sizeof(from);
        n = recvfrom(s, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
        QueryPerformanceCounter(&end);
        if (from.sin_addr.s_addr != to.sin_addr.s_addr || from.sin_port != to.sin_port) n = 0;
    }
    closesocket(s);
    WSACleanup();

    if (n < static_cast<int>(kHeader + 5) || memcmp(buf, packet, kHeader) != 0) return info;

    size_t pos = kHeader;
    const size_t size = static_cast<size_t>(n);
    info.password = buf[pos++] != 0;
    uint16_t players = 0, maxPlayers = 0;
    memcpy(&players, buf + pos, 2); pos += 2;
    memcpy(&maxPlayers, buf + pos, 2); pos += 2;
    if (!ReadString(buf, size, pos, info.hostname) ||
        !ReadString(buf, size, pos, info.gamemode) ||
        !ReadString(buf, size, pos, info.language))
        return info;

    info.players = players;
    info.maxPlayers = maxPlayers;
    info.pingMs = static_cast<int>((end.QuadPart - start.QuadPart) * 1000 / freq.QuadPart);
    info.online = true;
    return info;
}

std::string ServerInfoToJson(const std::vector<ServerInfo>& list) {
    std::string out = "[";
    for (size_t i = 0; i < list.size(); ++i) {
        const auto& s = list[i];
        if (i) out += ',';
        out += "{\"address\":";  AppendJsonString(out, s.address);
        out += ",\"online\":";   out += s.online ? "true" : "false";
        out += ",\"password\":"; out += s.password ? "true" : "false";
        out += ",\"players\":"   + std::to_string(s.players);
        out += ",\"maxPlayers\":" + std::to_string(s.maxPlayers);
        out += ",\"ping\":"      + std::to_string(s.pingMs);
        out += ",\"name\":";     AppendJsonString(out, s.hostname);
        out += ",\"mode\":";     AppendJsonString(out, s.gamemode);
        out += ",\"language\":"; AppendJsonString(out, s.language);
        out += '}';
    }
    out += ']';
    return out;
}

} // namespace skr::lobby
