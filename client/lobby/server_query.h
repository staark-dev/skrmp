#pragma once
// SA-MP / open.mp server query (UDP "SAMP" packet, opcode 'i'), used by the
// lobby to show live name, players and ping for each listed server.
#include <cstdint>
#include <string>
#include <vector>

namespace skr::lobby {

struct ServerInfo {
    std::string address;      // "host:port" as listed
    bool online = false;
    bool password = false;
    int players = 0;
    int maxPlayers = 0;
    int pingMs = 0;
    std::string hostname;     // UTF-8
    std::string gamemode;     // UTF-8
    std::string language;     // UTF-8
    std::string version;      // UTF-8
    std::string website;      // UTF-8
    std::string map;          // UTF-8
    std::string banners;      // UTF-8; optional server-supplied rule
    std::string bannerDark;   // optional custom rule (not in standard UDP query)
    std::string bannerLight;  // optional custom rule (not in standard UDP query)
    std::string allowedClients; // optional allowed_clients rule
    std::string artwork;      // optional artwork rule, raw server value
    std::string logo;         // UTF-8
    std::string discord;      // UTF-8
    bool rulesKnown = false;  // the 'r' (rules) reply arrived
    std::string skrClient;    // value of the "skr_client" rule; empty = no native SKR support
};

// Blocking; call off the game thread. `timeoutMs` per server.
ServerInfo QueryServer(const std::string& address, unsigned timeoutMs, bool includeRules = true);

// JSON array of measured server info + optional metadata from UDP rules.
// skr: true | false | null (null = rules unknown).
std::string ServerInfoToJson(const std::vector<ServerInfo>& list);

} // namespace skr::lobby
