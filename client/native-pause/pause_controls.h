#pragma once

#include <cstddef>
#include <cstdint>
#include <windows.h>

#include "native-pause/pause_controls_logic.h"

namespace skr::native_pause::controls {

enum class Category : uint8_t { OnFoot, Vehicle, Shared };
enum class BindingSlot : uint8_t { Primary = 0, Secondary = 1, Mouse = 2 };

struct ActionDescriptor {
    int id;
    const char* label;
    Category category;
    logic::ActionContext context;
    bool editable;
};

int ActionCount();
const ActionDescriptor& ActionAt(int index);

bool ReadBinding(int action, BindingSlot slot, uint32_t& outCode);
void FormatBinding(int action, BindingSlot slot, char* out, size_t capacity);

bool BeginCapture(int action, BindingSlot slot);
void CancelCapture();
bool IsCapturing();
bool IsCaptureArmed();
const char* CaptureStatus();

// Called before normal pause navigation. Returns true when the message belongs
// to an active binding capture and must not reach ImGui, GTA, or SA-MP.
bool ProcessMessage(UINT msg, WPARAM wParam, LPARAM lParam);

bool ConflictPending();
const char* ConflictSummary();
bool ResolveConflict(bool replace);

// Full GTA default-control transaction: native reinitialisation, verified
// mouse defaults, GTA save, readback, and rollback on failure.
bool RestoreDefaults();
const char* LastOperationStatus();

} // namespace skr::native_pause::controls
