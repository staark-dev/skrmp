#include "render-d3d9/present_chain.h"

#include <atomic>

namespace skr::render::present_chain {
namespace {

struct SlotState {
    void**                vtable = nullptr;
    std::atomic<PresentFn> original{nullptr};
    std::atomic<uint32_t>  frames{0};
};

SlotState g_slots[kSlotCount];
std::atomic<FrameHandler> g_handler{nullptr};
thread_local int t_depth = 0;

HRESULT Dispatch(int slot, IDirect3DDevice9* device, const RECT* source, const RECT* dest,
                 HWND window, const RGNDATA* dirty) {
    const PresentFn next = g_slots[slot].original.load(std::memory_order_acquire);
    if (!next) return D3DERR_INVALIDCALL;
    const FrameHandler handler = g_handler.load(std::memory_order_acquire);
    if (t_depth > 0 || !handler) return next(device, source, dest, window, dirty);
    ++t_depth;
    g_slots[slot].frames.fetch_add(1, std::memory_order_relaxed);
    const HRESULT result = handler(next, device, source, dest, window, dirty);
    --t_depth;
    return result;
}

HRESULT WINAPI HookPrimary(IDirect3DDevice9* d, const RECT* s, const RECT* t, HWND w, const RGNDATA* r) {
    return Dispatch(Primary, d, s, t, w, r);
}
HRESULT WINAPI HookRechained(IDirect3DDevice9* d, const RECT* s, const RECT* t, HWND w, const RGNDATA* r) {
    return Dispatch(Rechained, d, s, t, w, r);
}

void* HookFor(int slot) {
    return slot == Primary ? reinterpret_cast<void*>(&HookPrimary)
                           : reinterpret_cast<void*>(&HookRechained);
}

bool WriteEntry(void** vtable, void* value) {
    DWORD old = 0;
    if (!VirtualProtect(&vtable[kPresentIndex], sizeof(void*), PAGE_READWRITE, &old)) return false;
    vtable[kPresentIndex] = value;
    VirtualProtect(&vtable[kPresentIndex], sizeof(void*), old, &old);
    FlushInstructionCache(GetCurrentProcess(), &vtable[kPresentIndex], sizeof(void*));
    return true;
}

} // namespace

const char* ToString(InstallResult r) {
    switch (r) {
        case InstallResult::Installed:   return "installed";
        case InstallResult::AlreadyOurs: return "already ours";
        case InstallResult::SlotInUse:   return "slot in use";
        case InstallResult::Failed:      return "failed";
    }
    return "?";
}

void SetFrameHandler(FrameHandler handler) { g_handler.store(handler, std::memory_order_release); }

bool IsOurHook(const void* entry) {
    return entry == HookFor(Primary) || entry == HookFor(Rechained);
}

InstallResult Install(void** vtable, Slot slot) {
    if (!vtable || slot < 0 || slot >= kSlotCount) return InstallResult::Failed;
    void* current = vtable[kPresentIndex];
    if (IsOurHook(current)) return InstallResult::AlreadyOurs;
    if (!current) return InstallResult::Failed;
    SlotState& s = g_slots[slot];
    if (s.vtable) return InstallResult::SlotInUse;
    // Publish the original before the hook can be reached.
    s.original.store(reinterpret_cast<PresentFn>(current), std::memory_order_release);
    if (!WriteEntry(vtable, HookFor(slot))) {
        s.original.store(nullptr, std::memory_order_release);
        return InstallResult::Failed;
    }
    s.vtable = vtable;
    return InstallResult::Installed;
}

PresentFn Original(Slot slot) { return g_slots[slot].original.load(std::memory_order_acquire); }
uint32_t FrameCount(Slot slot) { return g_slots[slot].frames.load(std::memory_order_relaxed); }
bool InFrame() { return t_depth > 0; }

void Uninstall() {
    // Rechained first: it may sit in the same vtable as Primary.
    for (int slot = kSlotCount - 1; slot >= 0; --slot) {
        SlotState& s = g_slots[slot];
        if (s.vtable && s.vtable[kPresentIndex] == HookFor(slot)) {
            const PresentFn original = s.original.load(std::memory_order_acquire);
            if (original) WriteEntry(s.vtable, reinterpret_cast<void*>(original));
        }
        // Keep `original` valid: a frame already inside our hook may still need it.
        s.vtable = nullptr;
        s.frames.store(0, std::memory_order_relaxed);
    }
}

} // namespace skr::render::present_chain
