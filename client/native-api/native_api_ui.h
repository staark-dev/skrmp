#pragma once
// Internal: Native Runtime API notification toasts and one modal confirm dialog.
// Rendered with ImGui draw lists in the D3D9 EndScene pass (same pass as the
// phone and GPS panel). GTA thread only.

#include <cstdint>

#include "protocol/native_api.h"

struct ImDrawList;
struct ImFont;

namespace skr::native_api::ui {

void PushNotification(protocol::api::NotifyKind kind, uint8_t durationSec, const char* text);

bool ConfirmActive();
uint32_t ConfirmCallId();
void OpenConfirm(uint32_t callId, const char* title, const char* message, uint8_t timeoutSec);
void CloseConfirm();   // withdrawn by the server: no answer is produced

// Player answer or client-side timeout, produced once per dialog. The caller
// sends it as a Reply from Tick (never from the window procedure).
bool TakeConfirmAnswer(uint32_t& callId, protocol::api::ReplyResult& result);

struct DrawContext {
    ImDrawList* dl;
    ImFont* fontHeading;    // rasterised near the target notification text size
    ImFont* fontBody;       // rasterised near the target notification text size
    ImFont* fontLarge;      // larger face for confirm dialog title
    float displayW, displayH;
    // Phone coexistence: when the phone is visible, notifications shift left.
    bool phoneVisible;
    float phoneLeft;    // phone body x0 (only when phoneVisible)
    float phoneTop;     // phone body y0
};

bool WantsRender();   // true when toasts or confirm dialog are pending
void Tick(uint64_t nowMs);
void DrawImGui(const DrawContext& ctx);
bool OnWndProc(unsigned int msg, uintptr_t wParam, intptr_t lParam);
void Reset();

} // namespace skr::native_api::ui
