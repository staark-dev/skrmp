#pragma once
#include "cef-bridge/cef_api_types.h"
#include <cstdint>
#include <functional>

namespace skr::cef {

// Event callback: receives event name and opaque arg list, returns true to stop propagation.
using EventHandler = int(__cdecl*)(const char* event, CefListValue* args);

// omp-cef local API only (skr_cef_emit_event_str): emit with one string arg.
using EmitEventStrFn = void(__cdecl*)(const char* event, const char* arg);

enum class BridgeState : uint8_t {
    Uninitialized,
    Ready,
    ShuttingDown
};

class CefBridge {
public:
    static CefBridge& Instance();

    void Initialize(InternalApi* api);
    // omp-cef local mode: JS->C++ handlers receive `args` as a const char*
    // (first JS string argument, or null) instead of a CefListValue*.
    void InitializeLocal(InternalApi* api, EmitEventStrFn emitStr);
    bool IsLocalMode() const { return localMode_; }
    void Shutdown();
    BridgeState State() const { return state_; }
    bool IsReady() const { return state_ == BridgeState::Ready; }

    // Browser management — IDs are caller-assigned, must be unique
    void CreateBrowser(uint32_t id, const char* url, bool hidden = false, bool focused = false);
    void DestroyBrowser(uint32_t id);
    void ShowBrowser(uint32_t id);
    void HideBrowser(uint32_t id);
    void FocusBrowser(uint32_t id);
    void UnfocusBrowser(uint32_t id);

    // Events
    void EmitEvent(const char* event, CefListValue* args);
    void Subscribe(const char* event, EventHandler callback);
    // Emits `event` with one string argument. Returns false when the active
    // API cannot carry string args (samp-cef path) — nothing is sent then.
    bool EmitEventString(const char* event, const char* arg);
    // Reads the string payload a handler received. Returns nullptr unless the
    // bridge runs in omp-cef local mode (samp-cef passes a real CefListValue).
    const char* LocalPayload(CefListValue* args) const;

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
    EmitEventStrFn emitStr_ = nullptr;
    bool localMode_ = false;
};

// Well-known browser IDs for SKR UI
namespace BrowserId {
    constexpr uint32_t kMainMenu    = 100;
    constexpr uint32_t kHud         = 101;
    constexpr uint32_t kSettings    = 102;
    constexpr uint32_t kAuth        = 103;
    constexpr uint32_t kEscMenu     = 104;
}

} // namespace skr::cef
