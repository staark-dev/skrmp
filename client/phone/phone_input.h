#pragma once
// PHONE-001 M2 — Phone input ownership and routing.
//
// Owns the phone key (default L), cursor management, and eligibility checks.
// Integrates into the existing pause_input chain — no new WndProc hook or
// input-hook chain. The phone never draws or consumes input while hidden.
//
// Safety invariant: when !OwnsInput() the phone never consumes a message
// and never holds cursor or DirectInput ownership.

#include "phone/phone_controller.h"
#include "phone/phone_renderer.h"
#include <cstdint>

namespace skr::phone::input {

// One-time setup (called once from native_pause::OnEndScene on first frame).
void Initialize();

// Shutdown — called from dllmain/SkrShutdown.
void Shutdown();

// Process a Win32 message. Returns true if consumed (phone ate it).
// Called from pause_input::HandleMessage at the phone's priority slot.
bool ProcessMessage(unsigned int msg, uintptr_t wParam, intptr_t lParam,
                    uint64_t now);

// Per-frame tick. Updates controller, manages cursor/SA-MP ownership.
// Called from native_pause::OnEndScene.
void Tick(uint64_t now);

// True when the phone wants DirectInput suppressed (for the existing
// ShouldOwnDirectInput check in pause_input).
bool WantsDirectInput();

// True when the phone UI should be rendered this frame.
bool WantsRender();

// Access the live controller, model, views, layout, fonts and hit regions
// for the renderer. Valid only while WantsRender() is true.
Controller& GetController();
Model& GetModel();
Views& GetViews();
const Layout& GetLayout();
const Fonts& GetFonts();
const FrameHits& GetHits();

// Update font faces when the backbuffer size changes (called from
// pause_renderer after font atlas rebuild).
void SetFonts(const Fonts& f);

// Store this frame's computed layout and hit regions so clicks received
// through ProcessMessage resolve against what was actually drawn.
void SetFrameResult(const Layout& layout, const FrameHits& hits);

// External events from the gamemode (via Gate adapter, future M4).
void OnIncomingCall(const char* name, const char* number, uint64_t now);
void OnCallEnded();
void OnPushNotification(const char* title, const char* text, uint64_t now);

// PHONE-M4F: incoming SMS banner (tap opens the conversation with `number`).
void OnSmsNotification(const char* title, const char* text, const char* number, uint8_t hue, uint64_t now);

// True while the phone is on screen (not hidden / suspended).
bool IsVisible();
// True while the Messages conversation with `number` is the visible page.
bool IsConversationVisible(const char* number);

} // namespace skr::phone::input
