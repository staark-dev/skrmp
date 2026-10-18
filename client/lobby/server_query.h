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
    bool rulesKnown = false;  // the 'r' (rules) reply arrived
    std::string skrClient;    // value of the "skr_client" rule; empty = no native SKR support
};

// Blocking; call off the game thread. `timeoutMs` per server.
ServerInfo QueryServer(const std::string& address, unsigned timeoutMs);

// JSON array of {address, online, password, players, maxPlayers, ping, name, mode, language,
// skr: true | false | null (null = rules unknown)}.
std::string ServerInfoToJson(const std::vector<ServerInfo>& list);

} // namespace skr::lobby
