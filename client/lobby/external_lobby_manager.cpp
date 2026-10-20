// External launcher variant: no CEF lobby, menu patches, input hooks or cover.
#include "lobby/lobby_manager.h"
#include "cef-local/cef_local_loader.h"
#include "escape-menu/escape_menu.h"
#include "protocol/session_info.h"
#include <atomic>
namespace skr::lobby {
    namespace { std::atomic<int> mode{-1}; std::atomic<bool> gameplay{false}; }
    void Initialize() { mode.store(-1); gameplay.store(false); }
    
    void Shutdown() {}
    void OnCefTick() {}
    void OnEndSceneAfterIntro(IDirect3DDevice9*) {
        // Preserve the existing server CEF integration without creating a lobby browser.
        if (IsSkrSession() && !skr::cef_local::IsLoaded()) skr::cef_local::TryLoad();
    }

    void OnNetGameState(int) {}
    void ResetEnhancedSession() { mode.store(-1); gameplay.store(false); skr::escape_menu::Disable(); }
    void OnSessionInfo(uint8_t value) {
        mode.store(value == static_cast<uint8_t>(skr::protocol::SessionMode::CefIntro) ? 1 : 0);
    }

    bool IsSkrSession() { return mode.load() >= 0; }
    void OnGameplayConfirmed(const char*) {
        gameplay.store(true);
        if (IsSkrSession()) skr::escape_menu::Enable();
    }

    void OnInWorldTick() {
        if (!gameplay.load()) OnGameplayConfirmed("external launcher");
        else if (IsSkrSession()) skr::escape_menu::Enable();
    }

    bool IsLobbyActive() { return false; }
    bool IsEnabled() { return true; }
} // namespace skr::lobby
