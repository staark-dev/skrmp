#pragma once
// PHONE-001 M3/M5.9 — CJ's physical phone: follows the phone controller,
// never competes with it.
//
// The physical side is GTA's own CTaskComplexUseMobilePhone (task 1600) in the
// ped's TASK_SECONDARY_PARTIAL_ANIM slot.  With the browsing pose (M5.9,
// default ON), phone_in is frozen at a configurable chest-height frame and
// model 330 is attached early so CJ holds the phone in a browsing position
// instead of the phone-to-ear phone_talk loop.  On close, the hold releases
// and phone_out plays normally.  The coordinator decides when to start, quit
// or abort the task through a Backend (GTA in game, a mock in tests).
//
// Ownership: the coordinator remembers the task pointer it created and treats
// a slot holding anything else as foreign (e.g. a server-set cellphone special
// action) — it never quits or aborts a task it did not create, and it never
// dereferences a remembered pointer: the backend compares it with the slot.

#include <cstdint>

namespace skr::phone::body {

class Backend {
public:
    virtual ~Backend() = default;
    // Local player ped, 0 when none exists (spawn, class selection, disconnect).
    virtual uintptr_t Ped() = 0;
    // Task in the partial-anim secondary slot, 0 when empty.
    virtual uintptr_t SlotTask(uintptr_t ped) = 0;
    // Creates the phone task in the slot; returns it, or 0 on failure.
    virtual uintptr_t Start(uintptr_t ped) = 0;
    // Graceful: phone_out plays, GTA removes the model and ends the task.
    virtual void Quit(uintptr_t ped, uintptr_t task) = 0;
    // Immediate: weapon restored, model removed, slot cleared. False = refused.
    virtual bool Abort(uintptr_t ped, uintptr_t task) = 0;
    // Hold-pose experiment: freeze/release phone_in at atSec (no-op if absent).
    virtual void Hold(uintptr_t ped, bool on, float atSec) = 0;
};

struct Request {
    bool want = false;    // UI owns input AND the player is on foot and alive
    bool urgent = false;  // death / shutdown: skip phone_out
};

enum class Phase : uint8_t {
    Idle,       // no task of ours
    Active,     // our task running (phone_in / hold / phone_talk)
    Quitting,   // phone_out requested, waiting for GTA to end the task
    Foreign,    // UI open, slot used by someone else: UI only, no pose
};

const char* PhaseName(Phase p);

struct HoldPose {
    bool enabled = true;
    float atSec = 0.65f;   // browsing pose: freeze phone_in at chest height
};

class Coordinator {
public:
    explicit Coordinator(Backend& backend, HoldPose hold = {}) : b_(backend), hold_(hold) {}

    // Once per frame. Hidden phone with nothing owned returns before any read.
    void Update(const Request& r);
    // Shutdown / module teardown.
    void Release(bool urgent);
    void SetHoldPose(const HoldPose& h) { hold_ = h; }
    const HoldPose& GetHoldPose() const { return hold_; }

    Phase GetPhase() const { return phase_; }
    bool Owns() const { return task_ != 0; }
    uint32_t Starts() const { return starts_; }
    uint32_t Quits() const { return quits_; }
    uint32_t Aborts() const { return aborts_; }
    uint32_t Lost() const { return lost_; }    // our task vanished without us

private:
    void Forget();

    Backend& b_;
    HoldPose hold_;
    uintptr_t ped_ = 0;
    uintptr_t task_ = 0;
    bool quitSent_ = false;
    bool holding_ = false;
    bool sessionStarted_ = false;   // one Start per open session
    Phase phase_ = Phase::Idle;
    uint32_t starts_ = 0, quits_ = 0, aborts_ = 0, lost_ = 0;
};

} // namespace skr::phone::body
