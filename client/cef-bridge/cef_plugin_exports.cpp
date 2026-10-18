#include "cef-bridge/cef_plugin_exports.h"
#include "cef-bridge/cef_bridge.h"
#include "cef-bridge/cef_api_types.h"
#include "lobby/lobby_manager.h"

extern "C" {

__declspec(dllexport) void cef_initialize(void* api) {
    if (!api) return;
    auto* typed = static_cast<skr::cef::InternalApi*>(api);
    skr::cef::CefBridge::Instance().Initialize(typed);
}

__declspec(dllexport) void cef_samp_mainloop() {
    auto& bridge = skr::cef::CefBridge::Instance();
    if (!bridge.IsReady()) return;
    skr::lobby::OnCefTick();
}

__declspec(dllexport) void cef_quit() {
    skr::cef::CefBridge::Instance().Shutdown();
}

} // extern "C"
