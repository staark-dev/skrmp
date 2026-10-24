// PHONE-001 — SKR Street Phone: deterministic state + animation controller.
#include "phone/phone_controller.h"

#include <algorithm>

namespace skr::phone {

float EaseOutCubic(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    const float i = 1.0f - t;
    return 1.0f - i * i * i;
}

bool Controller::Open(uint64_t now) {
    if (state_ == State::Opening || state_ == State::Home || state_ == State::AppActive ||
        state_ == State::AppTransition)
        return false;
    if (state_ == State::Hidden || state_ == State::Suspended) shellT_ = 0.0f;
    // From Closing the slide reverses from its current value.
    state_ = State::Opening;
    last_ = now;
    return true;
}

bool Controller::Close(uint64_t now) {
    if (state_ == State::Hidden || state_ == State::Closing) return false;
    if (state_ == State::Suspended) { state_ = State::Hidden; return true; }
    // A running page transition is completed instantly; the device slides away.
    transT_ = 1.0f;
    kind_ = TransitionKind::None;
    pressed_ = kNoPress;
    state_ = State::Closing;
    last_ = now;
    return true;
}

void Controller::StartTransition(Screen to, TransitionKind kind, bool forward, uint64_t now) {
    previous_ = current_;
    current_ = to;
    kind_ = kind;
    forward_ = forward;
    transT_ = 0.0f;
    pressed_ = kNoPress;
    state_ = State::AppTransition;
    last_ = now;
}

bool Controller::OpenApp(AppId app, uint64_t now) {
    if (state_ != State::Home || app == AppId::Home || app >= AppId::Count) return false;
    StartTransition({app, 0}, TransitionKind::Zoom, true, now);
    return true;
}

bool Controller::OpenPage(uint8_t page, uint64_t now) {
    if (state_ != State::AppActive || page <= current_.page) return false;
    StartTransition({current_.app, page}, TransitionKind::Slide, true, now);
    return true;
}

bool Controller::Back(uint64_t now) {
    if (callSince_) return false;                    // the call screen answers ESC itself
    if (state_ == State::AppActive) {
        if (current_.page > 0)
            StartTransition({current_.app, static_cast<uint8_t>(current_.page - 1)}, TransitionKind::Slide, false, now);
        else
            StartTransition({AppId::Home, 0}, TransitionKind::Zoom, false, now);
        return true;
    }
    if (state_ == State::Home || state_ == State::Opening) return Close(now);
    return false;                                    // ignored mid-transition / closing
}

bool Controller::Jump(Screen to, uint64_t now) {
    if ((state_ != State::Home && state_ != State::AppActive) || to.app >= AppId::Count || to == current_)
        return false;
    if (to.app == current_.app)
        StartTransition(to, TransitionKind::Slide, to.page > current_.page, now);
    else
        StartTransition(to, TransitionKind::Zoom, to.app != AppId::Home, now);
    return true;
}

bool Controller::GoHome(uint64_t now) {
    if (state_ != State::AppActive) return false;
    StartTransition({AppId::Home, 0}, TransitionKind::Zoom, false, now);
    return true;
}

void Controller::Suspend() {
    if (!Visible()) return;
    transT_ = 1.0f;
    kind_ = TransitionKind::None;
    pressed_ = kNoPress;
    shellT_ = 0.0f;
    state_ = State::Suspended;
}

bool Controller::Resume(uint64_t now) {
    if (state_ != State::Suspended) return false;
    return Open(now);
}

void Controller::Reset() {
    *this = Controller(d_);
}

void Controller::BeginIncomingCall(uint64_t now) {
    callSince_ = now ? now : 1;
    if (!Visible() || state_ == State::Closing) Open(now);
}

void Controller::EndCall() { callSince_ = 0; }

void Controller::PushNotification(uint64_t now) { notifyAt_ = now ? now : 1; }

void Controller::Press(uint16_t target, uint64_t now) {
    if (!AcceptsClicks()) return;
    pressed_ = target;
    pressHeld_ = true;
    pressReleasedAt_ = now;
}

void Controller::Release(uint64_t now) {
    if (!pressHeld_) return;
    pressHeld_ = false;
    pressReleasedAt_ = now;
}

void Controller::Update(uint64_t now) {
    if (state_ == State::Hidden || state_ == State::Suspended) { last_ = now; return; }
    const float dt = static_cast<float>(now >= last_ ? now - last_ : 0);
    last_ = now;

    switch (state_) {
        case State::Opening:
            shellT_ = (std::min)(1.0f, shellT_ + dt / static_cast<float>(std::max<uint32_t>(d_.openMs, 1)));
            if (shellT_ >= 1.0f) state_ = current_.app == AppId::Home ? State::Home : State::AppActive;
            break;
        case State::Closing:
            shellT_ = (std::max)(0.0f, shellT_ - dt / static_cast<float>(std::max<uint32_t>(d_.closeMs, 1)));
            if (shellT_ <= 0.0f) {
                state_ = State::Hidden;
                callSince_ = 0;
            }
            break;
        case State::AppTransition: {
            const uint32_t ms = kind_ == TransitionKind::Zoom ? d_.appMs : d_.pageMs;
            transT_ = (std::min)(1.0f, transT_ + dt / static_cast<float>(std::max<uint32_t>(ms, 1)));
            if (transT_ >= 1.0f) {
                kind_ = TransitionKind::None;
                state_ = current_.app == AppId::Home ? State::Home : State::AppActive;
            }
            break;
        }
        default:
            break;
    }
    if (pressed_ != kNoPress && !pressHeld_ && now - pressReleasedAt_ >= d_.pressMs) pressed_ = kNoPress;
    if (notifyAt_ && now - notifyAt_ > static_cast<uint64_t>(d_.notifyInMs) + d_.notifyHoldMs + d_.notifyOutMs)
        notifyAt_ = 0;
}

bool Controller::OwnsInput() const {
    return state_ == State::Opening || state_ == State::Home || state_ == State::AppActive ||
           state_ == State::AppTransition;
}

bool Controller::Animating() const {
    return state_ == State::Opening || state_ == State::Closing || state_ == State::AppTransition ||
           notifyAt_ != 0 || callSince_ != 0 || pressed_ != kNoPress;
}

float Controller::Alpha() const {
    // Fully opaque once 55 % of the slide has passed; fades with the slide.
    return std::clamp(shellT_ / 0.55f, 0.0f, 1.0f);
}

float Controller::PressAmount(uint16_t target, uint64_t now) const {
    if (target != pressed_) return 0.0f;
    if (pressHeld_) return 1.0f;
    const float t = static_cast<float>(now - pressReleasedAt_) / static_cast<float>(std::max<uint32_t>(d_.pressMs, 1));
    return std::clamp(1.0f - t, 0.0f, 1.0f);
}

float Controller::NotifyAmount(uint64_t now) const {
    if (!notifyAt_ || now < notifyAt_) return 0.0f;
    const float e = static_cast<float>(now - notifyAt_);
    const float in = static_cast<float>(d_.notifyInMs), hold = static_cast<float>(d_.notifyHoldMs);
    const float out = static_cast<float>(d_.notifyOutMs);
    if (e < in) return EaseOutCubic(e / in);
    if (e < in + hold) return 1.0f;
    if (e < in + hold + out) return 1.0f - EaseOutCubic((e - in - hold) / out);
    return 0.0f;
}

float Controller::RingPhase(uint64_t now) const {
    if (!callSince_ || now < callSince_) return 0.0f;
    const uint64_t p = std::max<uint32_t>(d_.ringPeriodMs, 1);
    return static_cast<float>((now - callSince_) % p) / static_cast<float>(p);
}

} // namespace skr::phone
