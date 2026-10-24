// PHONE-001 M3/M5.9 — GTA SA 1.0 US backend for the physical phone.
//
// Every address below is from plugin-sdk (VALIDATE_OFFSET / converted-from
// comments) and was checked against the 1.0 US binary (PE 0x427101CA):
//   CTask::operator new          0x61A5A0  (cdecl size)
//   CTaskComplexUseMobilePhone   0x6348A0  ctor(int duration), size 0x20, GetId 0x640
//       m_bQuit +0x1D; vtable[6] MakeAbortable(ped, priority, event)
//       CreateFirstSubTask saves the weapon (+0x70C) and holsters it;
//       ControlSubTask adds model 330 at phone_in >= 0.85 s and removes it
//       (restoring the weapon) at phone_out >= ~1.566 s;
//       MakeAbortable(IMMEDIATE=2) removes the model and restores the weapon.
//   CTaskManager::SetTaskSecondary 0x681B60 (deletes the previous slot task)
//   RpAnimBlendClumpGetAssociation 0x4D68B0 (cdecl clump, animId)
//   CPed::AddWeaponModel          0x5E5ED0 (__thiscall ped, int modelId)
//   CPed::m_pIntelligence +0x47C, CPedIntelligence::m_TaskMgr +0x4,
//   CTaskManager::m_aSecondaryTasks +0x14, TASK_SECONDARY_PARTIAL_ANIM = 4,
//   CEntity::m_pRwClump +0x18, CAnimBlendAssociation m_fCurrentTime +0x20,
//   m_fSpeed +0x24, ANIM_DEFAULT_PHONE_IN = 0x91.
// Quit mirrors GTA's own quit helper (0x634A40): set m_bQuit, then a
// LEISURE MakeAbortable, which lets phone_talk end into phone_out.
//
// M5.9 browsing pose: phone_in is frozen at a configurable chest-height
// frame (default 0.65 s) instead of letting phone_talk (ear) play.  When
// the freeze point is below the natural 0.85 s model time, Hold() calls
// CPed::AddWeaponModel(330) so the phone is visible in the frozen hand.
// On close, the hold releases, phone_in resumes to phone_out, and GTA's
// task removes the model as usual.  On abort, MakeAbortable(IMMEDIATE)
// unconditionally removes the weapon model and restores the saved weapon.

#include "phone/phone_body_gta.h"

#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <cstdint>
#include <cwchar>

namespace skr::phone::body {
namespace {

constexpr uintptr_t kPlayersPed          = 0xB7CD98;
constexpr uintptr_t kTaskNew             = 0x61A5A0;
constexpr uintptr_t kUseMobilePhoneCtor  = 0x6348A0;
constexpr uintptr_t kSetTaskSecondary    = 0x681B60;
constexpr uintptr_t kGetAssociation      = 0x4D68B0;

constexpr uintptr_t kPedIntelligence     = 0x47C;
constexpr uintptr_t kPedClump            = 0x18;
constexpr uintptr_t kIntelTaskMgr        = 0x4;
constexpr uintptr_t kMgrSecondary        = 0x14;
constexpr int       kSlotPartialAnim     = 4;
constexpr uint32_t  kTaskSize            = 0x20;
constexpr int       kTaskUseMobilePhone  = 0x640;
constexpr uintptr_t kTaskQuitFlag        = 0x1D;
constexpr int       kVtGetId             = 4;
constexpr int       kVtMakeAbortable     = 6;
constexpr int       kAbortLeisure        = 0;
constexpr int       kAbortImmediate      = 2;
constexpr int       kAnimPhoneIn         = 0x91;
constexpr uintptr_t kAssocTime           = 0x20;
constexpr uintptr_t kAssocSpeed          = 0x24;
constexpr uintptr_t kAddWeaponModel      = 0x5E5ED0;
constexpr float     kNaturalModelTime    = 0.85f;
constexpr int       kCellphoneModel      = 330;

using TaskNewFn     = void*(__cdecl*)(uint32_t);
using CtorFn        = void*(__thiscall*)(void*, int);
using SetSecondFn   = void(__thiscall*)(void*, void*, int);
using GetIdFn       = int(__thiscall*)(void*);
using AbortableFn   = bool(__thiscall*)(void*, void*, int, void*);
using GetAssocFn    = void*(__cdecl*)(void*, int);
using AddWeaponModelFn = void(__thiscall*)(void*, int);

uintptr_t TaskManager(uintptr_t ped) {
    const uintptr_t intel = *reinterpret_cast<const uintptr_t*>(ped + kPedIntelligence);
    return intel ? intel + kIntelTaskMgr : 0;
}

uintptr_t* SlotAddress(uintptr_t ped) {
    const uintptr_t mgr = TaskManager(ped);
    return mgr ? reinterpret_cast<uintptr_t*>(mgr + kMgrSecondary + kSlotPartialAnim * sizeof(uintptr_t))
               : nullptr;
}

void* VFunc(uintptr_t object, int index) {
    const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(object);
    return *reinterpret_cast<void* const*>(vt + index * sizeof(void*));
}

// ── SEH-guarded engine calls (no C++ objects in these frames) ────────────
uintptr_t GuardedPed() {
    __try { return *reinterpret_cast<const volatile uintptr_t*>(kPlayersPed); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

uintptr_t GuardedSlot(uintptr_t ped) {
    __try {
        uintptr_t* slot = SlotAddress(ped);
        return slot ? *slot : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

uintptr_t GuardedStart(uintptr_t ped) {
    __try {
        const uintptr_t mgr = TaskManager(ped);
        if (!mgr) return 0;
        void* mem = reinterpret_cast<TaskNewFn>(kTaskNew)(kTaskSize);
        if (!mem) return 0;
        void* task = reinterpret_cast<CtorFn>(kUseMobilePhoneCtor)(mem, -1);  // -1: until quit
        reinterpret_cast<SetSecondFn>(kSetTaskSecondary)(reinterpret_cast<void*>(mgr), task,
                                                         kSlotPartialAnim);
        return GuardedSlot(ped) == reinterpret_cast<uintptr_t>(task) ? reinterpret_cast<uintptr_t>(task) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

bool IsOurPhoneTask(uintptr_t ped, uintptr_t task) {
    if (!task || GuardedSlot(ped) != task) return false;
    __try {
        return reinterpret_cast<GetIdFn>(VFunc(task, kVtGetId))(reinterpret_cast<void*>(task)) ==
               kTaskUseMobilePhone;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void GuardedQuit(uintptr_t ped, uintptr_t task) {
    __try {
        auto* quit = reinterpret_cast<volatile uint8_t*>(task + kTaskQuitFlag);
        if (*quit) return;
        *quit = 1;
        reinterpret_cast<AbortableFn>(VFunc(task, kVtMakeAbortable))(
            reinterpret_cast<void*>(task), reinterpret_cast<void*>(ped), kAbortLeisure, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

bool GuardedAbort(uintptr_t ped, uintptr_t task) {
    __try {
        const bool ok = reinterpret_cast<AbortableFn>(VFunc(task, kVtMakeAbortable))(
            reinterpret_cast<void*>(task), reinterpret_cast<void*>(ped), kAbortImmediate, nullptr);
        if (!ok) return false;
        const uintptr_t mgr = TaskManager(ped);
        if (mgr && GuardedSlot(ped) == task)
            reinterpret_cast<SetSecondFn>(kSetTaskSecondary)(reinterpret_cast<void*>(mgr), nullptr,
                                                             kSlotPartialAnim);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void GuardedHold(uintptr_t ped, bool on, float atSec) {
    __try {
        void* clump = *reinterpret_cast<void* const*>(ped + kPedClump);
        if (!clump) return;
        const uintptr_t assoc = reinterpret_cast<uintptr_t>(
            reinterpret_cast<GetAssocFn>(kGetAssociation)(clump, kAnimPhoneIn));
        if (!assoc) return;   // phone_in already finished or not started yet
        auto* time = reinterpret_cast<volatile float*>(assoc + kAssocTime);
        auto* speed = reinterpret_cast<volatile float*>(assoc + kAssocSpeed);
        if (!on) {
            if (*speed == 0.0f) *speed = 1.0f;
            return;
        }
        if (*time >= atSec && *speed != 0.0f) {
            *time = atSec;
            *speed = 0.0f;
            // Browsing pose: the task adds model 330 at phone_in 0.85 s, but
            // we freeze earlier. Attach the phone model now so CJ holds it
            // at the browsing-height frame instead of having an empty hand.
            if (atSec < kNaturalModelTime)
                reinterpret_cast<AddWeaponModelFn>(kAddWeaponModel)(
                    reinterpret_cast<void*>(ped), kCellphoneModel);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

} // namespace

uintptr_t GtaBackend::Ped() { return GuardedPed(); }

uintptr_t GtaBackend::SlotTask(uintptr_t ped) { return ped ? GuardedSlot(ped) : 0; }

uintptr_t GtaBackend::Start(uintptr_t ped) {
    if (!ped || GuardedSlot(ped)) return 0;   // never replace a task we did not create
    return GuardedStart(ped);
}

void GtaBackend::Quit(uintptr_t ped, uintptr_t task) {
    if (IsOurPhoneTask(ped, task)) GuardedQuit(ped, task);
}

bool GtaBackend::Abort(uintptr_t ped, uintptr_t task) {
    return IsOurPhoneTask(ped, task) && GuardedAbort(ped, task);
}

void GtaBackend::Hold(uintptr_t ped, bool on, float atSec) {
    if (ped) GuardedHold(ped, on, atSec);
}

HoldPose LoadHoldPose() {
    HoldPose h;
    PWSTR local = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)) || !local) return h;
    wchar_t path[MAX_PATH] = {};
    swprintf(path, MAX_PATH, L"%s\\SKR\\client.ini", local);
    CoTaskMemFree(local);
    h.enabled = GetPrivateProfileIntW(L"Phone", L"HoldPose", 1, path) != 0;
    const int ms = static_cast<int>(GetPrivateProfileIntW(L"Phone", L"HoldPoseMs", 650, path));
    h.atSec = static_cast<float>((std::clamp)(ms, 350, 1500)) / 1000.0f;
    return h;
}

} // namespace skr::phone::body
