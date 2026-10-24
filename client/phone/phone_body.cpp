// PHONE-001 M3 — physical phone coordinator (pure; see phone_body.h).
#include "phone/phone_body.h"

namespace skr::phone::body {

const char* PhaseName(Phase p) {
    switch (p) {
        case Phase::Idle:     return "IDLE";
        case Phase::Active:   return "ACTIVE";
        case Phase::Quitting: return "QUITTING";
        case Phase::Foreign:  return "FOREIGN";
    }
    return "?";
}

void Coordinator::Forget() {
    ped_ = 0;
    task_ = 0;
    quitSent_ = false;
    holding_ = false;
}

void Coordinator::Update(const Request& r) {
    if (!r.want) sessionStarted_ = false;
    if (!task_ && !r.want) {
        phase_ = Phase::Idle;
        return;
    }

    const uintptr_t ped = b_.Ped();
    if (task_ && ped != ped_) {
        // Ped destroyed or replaced (respawn, reconnect): the old task died
        // with it. Never touch the old memory.
        Forget();
        ++lost_;
    }
    if (!ped) {
        phase_ = Phase::Idle;
        return;
    }
    if (task_ && b_.SlotTask(ped) != task_) {
        // GTA finished phone_out, or flushed the slot (death, ClearAnimations,
        // vehicle entry). Restart only in a new open session.
        if (!quitSent_) ++lost_;
        Forget();
    }

    if (task_) {
        if (r.urgent) {
            if (b_.Abort(ped, task_)) {
                ++aborts_;
                Forget();
                phase_ = Phase::Idle;
                return;
            }
            // Refused: fall back to the graceful exit, never leave it running.
        }
        if (!r.want || r.urgent) {
            if (holding_) {
                b_.Hold(ped, false, 0.0f);
                holding_ = false;
            }
            if (!quitSent_) {
                b_.Quit(ped, task_);
                quitSent_ = true;
                ++quits_;
            }
            phase_ = Phase::Quitting;
            return;
        }
        if (quitSent_) {
            // phone_out cannot be cancelled; a reopen restarts once it ends.
            phase_ = Phase::Quitting;
            return;
        }
        if (hold_.enabled) {
            b_.Hold(ped, true, hold_.atSec);
            holding_ = true;
        }
        phase_ = Phase::Active;
        return;
    }

    if (!r.want) {
        phase_ = Phase::Idle;
        return;
    }
    if (r.urgent || sessionStarted_) {
        phase_ = sessionStarted_ ? Phase::Foreign : Phase::Idle;
        return;
    }
    if (b_.SlotTask(ped)) {
        phase_ = Phase::Foreign;
        return;
    }
    const uintptr_t t = b_.Start(ped);
    sessionStarted_ = true;
    if (!t) {
        phase_ = Phase::Foreign;
        return;
    }
    ped_ = ped;
    task_ = t;
    ++starts_;
    phase_ = Phase::Active;
}

void Coordinator::Release(bool urgent) {
    Request r;
    r.want = false;
    r.urgent = urgent;
    Update(r);
    if (task_ && urgent) Forget();   // refused abort at teardown: GTA owns it now
}

} // namespace skr::phone::body
