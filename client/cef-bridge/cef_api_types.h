#pragma once
#include <cstdint>

// C ABI types matching samp-cef's InternalApi struct.
// These are derived from the Rust source at commit f14fe4fa.
// Do NOT modify without verifying against the actual samp-cef build.

namespace skr::cef {

extern "C" {

using CefListValue = void; // opaque — matches cef_list_value_t*

using CreateBrowserFn   = void(__cdecl*)(uint32_t id, const char* url, bool hidden, bool focused);
using DestroyBrowserFn  = void(__cdecl*)(uint32_t id);
using HideBrowserFn     = void(__cdecl*)(uint32_t id, bool hide);
using FocusBrowserFn    = void(__cdecl*)(uint32_t id, bool focus);
using CreateListFn      = CefListValue*(__cdecl*)();
using EmitEventFn       = void(__cdecl*)(const char* event, CefListValue* list);
using SubscribeFn       = void(__cdecl*)(const char* event, void* callback);
using InputAvailableFn  = bool(__cdecl*)(uint32_t browser);
using CefReadyFn        = bool(__cdecl*)();
using TryFocusBrowserFn = bool(__cdecl*)(uint32_t browser);
using BrowserExistsFn   = bool(__cdecl*)(uint32_t browser);
using BrowserReadyFn    = bool(__cdecl*)(uint32_t browser);
using OnBrowserReadyFn  = void(__cdecl*)(uint32_t browser, void* callback);
using GtaWindowActiveFn = bool(__cdecl*)();

} // extern "C"

// Matches the InternalApi repr(C) struct from samp-cef cef-api
struct InternalApi {
    CreateBrowserFn   cef_create_browser;
    DestroyBrowserFn  cef_destroy_browser;
    HideBrowserFn     cef_hide_browser;
    FocusBrowserFn    cef_focus_browser;
    CreateListFn      cef_create_list;
    EmitEventFn       cef_emit_event;
    SubscribeFn       cef_subscribe;
    InputAvailableFn  cef_input_available;
    CefReadyFn        cef_ready;
    TryFocusBrowserFn cef_try_focus_browser;
    BrowserExistsFn   cef_browser_exists;
    BrowserReadyFn    cef_browser_ready;
    OnBrowserReadyFn  cef_on_browser_ready;
    GtaWindowActiveFn cef_gta_window_active;
};

} // namespace skr::cef
