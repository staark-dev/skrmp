#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>

namespace skr::launcher {
struct LaunchConfig;

struct LobbyResult {
    bool quit = false;
    std::string serverAddress;   // "ip:port"
    std::string nickname;
    bool smokePassed = false;
};

// Shows a full-screen borderless WebView2 window with the lobby UI.
// Blocks until the user connects to a server or quits.
// lobbyDir: folder containing lobby.html, lobby.css, lobby.js, assets/
LobbyResult ShowLobbyWindow(HINSTANCE hInstance, const std::wstring& lobbyDir, bool smoke = false,
                           const LaunchConfig* launchConfig = nullptr);

} // namespace skr::launcher
