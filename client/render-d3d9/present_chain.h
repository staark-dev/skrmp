#pragma once
// ═══════════════════════════════════════════════════════════════════════
// IDirect3DDevice9::Present hook chain (vtable index 17)
// ═══════════════════════════════════════════════════════════════════════
// SA-MP 0.3.7 replaces GTA's device with a proxy whose vtable we do not
// own, so Present must be hooked twice: on the original vtable (Primary)
// and on the proxy vtable after SA-MP initialises (Rechained). Either hook
// may call into the other (proxy → real device), so per-frame SKR work runs
// only at the outermost hook on a thread; nested calls pass straight to the
// original. Installing never targets an entry that already holds our hook.

#include <d3d9.h>
#include <cstdint>

namespace skr::render::present_chain {

using PresentFn = HRESULT(WINAPI*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);

// Per-frame SKR work. Must call `next` exactly once and return its result.
using FrameHandler = HRESULT (*)(PresentFn next, IDirect3DDevice9* device, const RECT* source,
                                 const RECT* dest, HWND window, const RGNDATA* dirty);

enum Slot : int { Primary = 0, Rechained = 1, kSlotCount = 2 };

enum class InstallResult : uint8_t {
    Installed,      // entry now holds our hook; previous entry kept as the slot's original
    AlreadyOurs,    // entry already holds one of our hooks: nothing done (no duplicate)
    SlotInUse,      // this slot is installed elsewhere
    Failed,         // null vtable/entry or VirtualProtect failure
};
const char* ToString(InstallResult r);

constexpr int kPresentIndex = 17;

void          SetFrameHandler(FrameHandler handler);
InstallResult Install(void** vtable, Slot slot);
bool          IsOurHook(const void* entry);
PresentFn     Original(Slot slot);
uint32_t      FrameCount(Slot slot);     // outermost frames handled through this slot
bool          InFrame();                 // true inside the frame handler on this thread
void          Uninstall();               // restores entries that still hold our hooks

} // namespace skr::render::present_chain
