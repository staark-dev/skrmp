#pragma once
// PHONE-001 M3/M5.9 — GTA SA 1.0 US backend for phone_body.
// Uses CTaskComplexUseMobilePhone with a browsing-height freeze (M5.9).

#include "phone/phone_body.h"

namespace skr::phone::body {

class GtaBackend final : public Backend {
public:
    uintptr_t Ped() override;
    uintptr_t SlotTask(uintptr_t ped) override;
    uintptr_t Start(uintptr_t ped) override;
    void Quit(uintptr_t ped, uintptr_t task) override;
    bool Abort(uintptr_t ped, uintptr_t task) override;
    void Hold(uintptr_t ped, bool on, float atSec) override;
};

// [Phone] HoldPose=0|1, HoldPoseMs=860..1500 in %LOCALAPPDATA%\SKR\client.ini.
HoldPose LoadHoldPose();

} // namespace skr::phone::body
