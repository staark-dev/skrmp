#pragma once
// PHONE-001 — SKR Street Phone: deterministic state + animation controller.
//
// One state machine owns every phone animation, so transitions never compete:
//
//   HIDDEN ──Open──▶ OPENING ──done──▶ HOME ⇄ APP_TRANSITION ⇄ APP_ACTIVE
//     ▲                 │  ▲                                     │
//     └──done── CLOSING ◀─┘  └── reversal keeps the eased value ──┘
//   any visible state ──Suspend──▶ SUSPENDED ──Resume──▶ OPENING
//   Reset() (death / disconnect / shutdown) ──▶ HIDDEN, app state cleared
//
// Time comes from the caller in milliseconds (GetTickCount64 in game, a fixed
// clock in tests/previews). Progress advances by elapsed time / duration, so
// the result is identical at 30, 60 or 144 fps. The open and close slide use
// the SAME curve (out-cubic of progress), so reversing an interrupted open or
// close continues from the exact on-screen position with no jump.
//
// No allocation, no I/O, no game access. Update() is a no-op while HIDDEN.

#include <cstdint>

namespace skr::phone {

enum class State : uint8_t {
    Hidden = 0,
    Opening,
    Home,
    AppActive,
    AppTransition,
    Closing,
    Suspended,
};

enum class AppId : uint8_t {
    Home = 0,
    Phone,
    Messages,
    Contacts,
    Gps,
    Settings,
    Count,
};

struct Durations {
    uint32_t openMs = 280;
    uint32_t closeMs = 220;
    uint32_t appMs = 240;       // home <-> app zoom
    uint32_t pageMs = 220;      // page <-> page slide inside an app
    uint32_t pressMs = 140;     // release decay of the tap feedback
    uint32_t notifyInMs = 240;
    uint32_t notifyHoldMs = 3600;
    uint32_t notifyOutMs = 260;
    uint32_t ringPeriodMs = 1500;
};

struct Screen {
    AppId app = AppId::Home;
    uint8_t page = 0;           // 0 = app root, 1+ = deeper pages (e.g. a conversation)
    bool operator==(const Screen& o) const { return app == o.app && page == o.page; }
};

enum class TransitionKind : uint8_t { None, Zoom, Slide };

float EaseOutCubic(float t);

class Controller {
public:
    explicit Controller(const Durations& d = {}) : d_(d) {}

    // Requests. Each returns false when ignored in the current state.
    bool Open(uint64_t now);
    bool Close(uint64_t now);
    bool Toggle(uint64_t now) { return Visible() && state_ != State::Closing ? Close(now) : Open(now); }
    bool OpenApp(AppId app, uint64_t now);          // from Home only
    bool OpenPage(uint8_t page, uint64_t now);      // deeper page of the current app
    bool Back(uint64_t now);                        // page -> app root -> Home -> close
    bool GoHome(uint64_t now);
    bool Jump(Screen to, uint64_t now);             // any app/page (notification, Contacts -> Messages)
    void Suspend();                                 // focus loss / pause / modal: hide now, keep screen
    bool Resume(uint64_t now);
    void Reset();                                   // death / disconnect / shutdown

    // Overlays.
    void BeginIncomingCall(uint64_t now);           // opens the phone if hidden
    void EndCall();
    void PushNotification(uint64_t now);
    void Press(uint16_t target, uint64_t now);
    void Release(uint64_t now);

    void Update(uint64_t now);

    // Queries (valid after Update).
    State GetState() const { return state_; }
    bool Visible() const { return state_ != State::Hidden && state_ != State::Suspended; }
    bool OwnsInput() const;                         // cursor + gameplay-input suppression wanted
    bool AcceptsClicks() const { return state_ == State::Home || state_ == State::AppActive; }
    bool Animating() const;

    float Slide() const { return EaseOutCubic(shellT_); }       // layout slide 0..1
    float Alpha() const;                                         // device alpha 0..1
    Screen Current() const { return current_; }
    Screen Previous() const { return previous_; }
    TransitionKind Kind() const { return kind_; }
    bool Forward() const { return forward_; }
    float Transition() const { return kind_ == TransitionKind::None ? 1.0f : EaseOutCubic(transT_); }
    float PressAmount(uint16_t target, uint64_t now) const;
    float NotifyAmount(uint64_t now) const;          // 0 hidden .. 1 fully shown
    bool CallIncoming() const { return callSince_ != 0; }
    float RingPhase(uint64_t now) const;             // 0..1, repeats every ringPeriodMs

    const Durations& Timing() const { return d_; }

private:
    static constexpr uint16_t kNoPress = 0xFFFF;
    void StartTransition(Screen to, TransitionKind kind, bool forward, uint64_t now);

    Durations d_;
    State state_ = State::Hidden;
    float shellT_ = 0.0f;       // linear progress toward open
    float transT_ = 1.0f;       // linear progress of the page transition
    uint64_t last_ = 0;
    Screen current_{}, previous_{};
    TransitionKind kind_ = TransitionKind::None;
    bool forward_ = true;
    uint16_t pressed_ = kNoPress;
    uint64_t pressReleasedAt_ = 0;
    bool pressHeld_ = false;
    uint64_t notifyAt_ = 0;
    uint64_t callSince_ = 0;
};

} // namespace skr::phone
