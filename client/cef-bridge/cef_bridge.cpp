#include "cef-bridge/cef_bridge.h"

namespace skr::cef {

CefBridge& CefBridge::Instance() {
    static CefBridge instance;
    return instance;
}

void CefBridge::Initialize(InternalApi* api) {
    if (!api || state_ != BridgeState::Uninitialized) return;
    api_ = api;
    state_ = BridgeState::Ready;
}

void CefBridge::InitializeLocal(InternalApi* api, EmitEventStrFn emitStr) {
    if (!api || state_ != BridgeState::Uninitialized) return;
    Initialize(api);
    emitStr_ = emitStr;
    localMode_ = true;
}

void CefBridge::Shutdown() {
    state_ = BridgeState::ShuttingDown;
    api_ = nullptr;
    emitStr_ = nullptr;
    localMode_ = false;
    state_ = BridgeState::Uninitialized;
}

void CefBridge::CreateBrowser(uint32_t id, const char* url, bool hidden, bool focused) {
    if (!IsReady() || !url) return;
    api_->cef_create_browser(id, url, hidden, focused);
}

void CefBridge::DestroyBrowser(uint32_t id) {
    if (!IsReady()) return;
    api_->cef_destroy_browser(id);
}

void CefBridge::ShowBrowser(uint32_t id) {
    if (!IsReady()) return;
    api_->cef_hide_browser(id, false);
}

void CefBridge::HideBrowser(uint32_t id) {
    if (!IsReady()) return;
    api_->cef_hide_browser(id, true);
}

void CefBridge::FocusBrowser(uint32_t id) {
    if (!IsReady()) return;
    api_->cef_focus_browser(id, true);
}

void CefBridge::UnfocusBrowser(uint32_t id) {
    if (!IsReady()) return;
    api_->cef_focus_browser(id, false);
}

void CefBridge::EmitEvent(const char* event, CefListValue* args) {
    if (!IsReady() || !event) return;
    api_->cef_emit_event(event, args);
}

void CefBridge::Subscribe(const char* event, EventHandler callback) {
    if (!IsReady() || !event) return;
    api_->cef_subscribe(event, reinterpret_cast<void*>(callback));
}

bool CefBridge::EmitEventString(const char* event, const char* arg) {
    if (!IsReady() || !event || !emitStr_) return false;
    emitStr_(event, arg);
    return true;
}

const char* CefBridge::LocalPayload(CefListValue* args) const {
    if (!localMode_) return nullptr;
    return static_cast<const char*>(args);
}

bool CefBridge::IsCefReady() const {
    if (!IsReady()) return false;
    return api_->cef_ready();
}

bool CefBridge::BrowserExists(uint32_t id) const {
    if (!IsReady()) return false;
    return api_->cef_browser_exists(id);
}

bool CefBridge::BrowserReady(uint32_t id) const {
    if (!IsReady()) return false;
    return api_->cef_browser_ready(id);
}

bool CefBridge::IsWindowActive() const {
    if (!IsReady()) return false;
    return api_->cef_gta_window_active();
}

} // namespace skr::cef
