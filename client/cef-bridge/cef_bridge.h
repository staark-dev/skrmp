#pragma once
#include "cef-bridge/cef_api_types.h"
#include <cstdint>
#include <functional>

namespace skr::cef {

// Event callback: receives event name and opaque arg list, returns true to stop propagation.
using EventHandler = int(__cdecl*)(const char* event, CefListValue* args);

enum class BridgeState : uint8_t {
    Uninitialized,
    Ready,
    ShuttingDown
};

class CefBridge {
public:
    static CefBridge& Instance();

    void Initialize(InternalApi* api);
    void Shutdown();
    BridgeState State() const { return state_; }
    bool IsReady() const { return state_ == BridgeState::Ready; }

    // Browser management — IDs are caller-assigned, must be unique
    void CreateBrowser(uint32_t id, const char* url, bool hidden = false, bool focused = false);
    void DestroyBrowser(uint32_t id);
    void ShowBrowser(uint32_t id);
    void HideBrowser(uint32_t id);
    void FocusBrowser(uint32_t id);

    // Events
    void EmitEvent(const char* event, CefListValue* args);
    void Subscribe(const char* event, EventHandler callback);

    // Queries
    bool IsCefReady() const;
    bool BrowserExists(uint32_t id) const;
    bool BrowserReady(uint32_t id) const;
    bool IsWindowActive() const;

    CefBridge(const CefBridge&) = delete;
    CefBridge& operator=(const CefBridge&) = delete;

private:
    CefBridge() = default;
    InternalApi* api_ = nullptr;
    BridgeState state_ = BridgeState::Uninitialized;
};

// Well-known browser IDs for SKR UI
namespace BrowserId {
    constexpr uint32_t kMainMenu    = 100;
    constexpr uint32_t kHud         = 101;
    constexpr uint32_t kSettings    = 102;
    constexpr uint32_t kAuth        = 103;
}

} // namespace skr::cef
