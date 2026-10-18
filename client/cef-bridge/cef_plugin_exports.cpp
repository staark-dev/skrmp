#include "cef-bridge/cef_plugin_exports.h"
#include "cef-bridge/cef_bridge.h"
#include "cef-bridge/cef_api_types.h"
#include "lobby/lobby_manager.h"

// samp-cef (commit f14fe4fa) only ever calls `cef_initialize` and `cef_quit`
// on a loaded client plugin — the `cef_samp_mainloop` export we had was a
// hopeful stub that never fired. OnCefTick is now driven from the D3D9 hook
// instead. The stub stays exported in case a future samp-cef build adds the
// callback, but no behavior depends on it any more.

extern "C" {

__declspec(dllexport) void cef_initialize(void* api) {
    if (!api) return;
    auto* typed = static_cast<skr::cef::InternalApi*>(api);
    skr::cef::CefBridge::Instance().Initialize(typed);
}

__declspec(dllexport) void cef_samp_mainloop() {
    // Reserved: samp-cef does not call this today. See the top-of-file note.
}

__declspec(dllexport) void cef_quit() {
    skr::cef::CefBridge::Instance().Shutdown();
}

} // extern "C"
