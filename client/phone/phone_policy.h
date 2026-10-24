#pragma once
// PHONE-001 M2 — pure open-eligibility and phone-key policy.
//
// No Win32, no game memory: phone_input samples the game into a GameView and
// feeds raw key messages through KeyEdge, so this logic is unit-testable.

#include "phone/phone_body.h"
#include "phone/phone_controller.h"
#include <cstdint>

namespace skr::phone::policy {

enum class Block : uint8_t {
    None = 0,
    NoPed,          // CPlayerPed not created yet (spawn / class selection)
    Dead,
    Driver,         // passengers may use the phone, drivers may not
    ChatOpen,
    DialogOpen,
};

inline const char* BlockName(Block b) {
    switch (b) {
        case Block::None:       return "none";
        case Block::NoPed:      return "no-player-ped";
        case Block::Dead:       return "player-dead";
        case Block::Driver:     return "driving";
        case Block::ChatOpen:   return "samp-chat-open";
        case Block::DialogOpen: return "samp-dialog-open";
    }
    return "unknown";
}

struct GameView {
    bool pedValid = false;
    bool alive = false;
    bool inVehicle = false;     // CPed::bInVehicle (pedFlags byte +0x46D bit 0)
    bool isDriver = false;      // CVehicle::m_pDriver == player ped
    bool chatOpen = false;
    bool dialogOpen = false;
};

inline Block Evaluate(const GameView& g) {
    if (!g.pedValid) return Block::NoPed;
    if (!g.alive) return Block::Dead;
    if (g.inVehicle && g.isDriver) return Block::Driver;
    if (g.chatOpen) return Block::ChatOpen;
    if (g.dialogOpen) return Block::DialogOpen;
    return Block::None;
}

// Per-frame rule while the phone is not Hidden (ped fields only).
enum class Running : uint8_t { Keep, Close, Reset };

inline Running WhileOpen(const GameView& ped) {
    if (!ped.pedValid || !ped.alive) return Running::Reset;   // death / ped destroyed
    if (ped.inVehicle && ped.isDriver) return Running::Close; // took the wheel
    return Running::Keep;
}

// What CJ's body should do this frame. The pose follows input ownership
// (Opening..AppTransition), so Closing / Suspended / Hidden all put it away.
// Passengers get the UI only: the seated phone pose is not verified.
inline body::Request BodyRequest(const Controller& c, const GameView& ped) {
    body::Request r;
    r.urgent = !ped.pedValid || !ped.alive;
    r.want = c.OwnsInput() && !r.urgent && !ped.inVehicle;
    return r;
}

// One physical press -> one edge. Auto-repeat (lParam bit 30) never counts.
// The same press delivered twice (a second subclass hook forwarding the same
// message) is rejected by the hold latch; the latch can never wedge the key,
// because a fresh non-repeat down after kRelatchMs is always accepted even if
// the key-up was swallowed by another window procedure.
struct KeyEdge {
    static constexpr uint64_t kRelatchMs = 60;
    bool held = false;
    uint64_t acceptedAt = 0;

    bool Down(uint32_t lParam, uint64_t now) {
        if ((lParam & (1u << 30)) != 0) return false;
        if (held && now - acceptedAt < kRelatchMs) return false;
        held = true;
        acceptedAt = now;
        return true;
    }
    void Up() { held = false; }
    void Reset() { held = false; }
};

// The renderer runs only while this is true; when it turns false the input
// module drops the last frame's hit regions so no stale click target remains.
inline bool ShouldRender(const Controller& c) { return c.Visible() || c.Animating(); }

enum class KeyResult : uint8_t { Opened, Closed, Blocked, Ignored };

// Phone-key toggle. A press during Closing reverses into Opening (the slide
// continues from its current position); Hidden and Suspended open fresh.
inline bool KeyWillClose(const Controller& c) {
    const State s = c.GetState();
    return s != State::Hidden && s != State::Suspended && s != State::Closing;
}

inline KeyResult OnPhoneKey(Controller& c, Block block, uint64_t now) {
    if (KeyWillClose(c)) return c.Close(now) ? KeyResult::Closed : KeyResult::Ignored;
    if (block != Block::None) return KeyResult::Blocked;
    return c.Open(now) ? KeyResult::Opened : KeyResult::Ignored;
}

// Accepted key edge -> toggle. Eligibility is sampled only when the press
// would open, so closing never depends on game memory.
template <class EvaluateFn>
inline KeyResult OnPhoneKeyEdge(Controller& c, uint64_t now, EvaluateFn&& evaluate) {
    return OnPhoneKey(c, KeyWillClose(c) ? Block::None : evaluate(), now);
}

} // namespace skr::phone::policy
