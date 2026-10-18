#pragma once
// Public server directory from open.mp (the list behind https://open.mp/servers).
#include <string>
#include <vector>

namespace skr::lobby {

struct PublicServer {
    std::string address;   // "host:port"
    std::string name;      // UTF-8
    std::string mode;
    std::string language;
    std::string version;
    int players = 0;
    int maxPlayers = 0;
    bool password = false;
};

// Blocking HTTPS GET of the open.mp directory; call off the game thread.
bool FetchOpenMpServers(std::vector<PublicServer>& out, std::string& error);

// JSON array in the shape the lobby's loadServers() expects, in list order.
std::string PublicServersToLobbyJson(const std::vector<PublicServer>& list);

} // namespace skr::lobby
