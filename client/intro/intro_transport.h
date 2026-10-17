#pragma once
#include "intro/intro_timeline.h"

// Shared audio/visual transport. Pure logic with injected clocks so it can be tested
// without a device: `now` is a monotonic wall clock in seconds; `audioTime` is the
// playback position reported by the audio backend, or < 0 when there is no audio.
//
// Two times are kept: the playback clock (audio position, or wall clock) and the intro
// time used for drawing. They are equal until the intro reaches kHoldAt while the world
// is not ready yet; the intro time then holds at kHoldAt (music keeps playing) and the
// difference is accumulated in HoldOffset().

namespace skr::intro {

class Transport {
public:
    enum class State { Idle, Playing, Paused, Finished };

    // Audio position that has not advanced for this long while playing is treated as a
    // dead device; time then continues from the monotonic clock.
    static constexpr double kAudioStallSec = 0.5;

    void Start(double now) {
        if (state_ != State::Idle) return;
        state_ = State::Playing;
        lastNow_ = now;
        lastAudioChange_ = now;
        clock_ = time_ = holdOffset_ = 0.0;
        keySeenUp_ = false;
    }

    void Pause() { if (state_ == State::Playing) state_ = State::Paused; }

    void Resume(double now) {
        if (state_ != State::Paused) return;
        state_ = State::Playing;
        lastNow_ = now;
        lastAudioChange_ = now;
    }

    void Finish() { state_ = State::Finished; }

    void SetWorldReady(bool ready) { worldReady_ = ready; }

    // Advances the clocks; returns the intro time to draw.
    double Update(double now, double audioTime) {
        if (state_ != State::Playing) { lastNow_ = now; return time_; }

        double wall = clock_ + (now - lastNow_);
        lastNow_ = now;

        if (audioTime >= 0.0 && !audioDead_) {
            if (audioTime != lastAudio_) { lastAudio_ = audioTime; lastAudioChange_ = now; }
            if (now - lastAudioChange_ > kAudioStallSec) {
                // Stalled: continue on the monotonic clock from the last audio position,
                // counting the stall itself so the picture does not jump back.
                audioDead_ = true;
                clock_ = (lastAudio_ > 0.0 ? lastAudio_ : 0.0) + (now - lastAudioChange_);
            } else {
                clock_ = audioTime;
            }
        } else {
            clock_ = wall;
        }

        double t = clock_ - holdOffset_;
        if (!worldReady_ && t > kHoldAt) {
            holdOffset_ = clock_ - kHoldAt;
            t = kHoldAt;
        }
        time_ = t;

        if (time_ >= kDuration) { time_ = kDuration; state_ = State::Finished; }
        return time_;
    }

    // Escape is honoured only after kSkipAfter and only once the key was seen released
    // after the intro started, so a key held before start never skips.
    bool OnSkipKey(bool keyDown) {
        if (state_ != State::Playing && state_ != State::Paused) return false;
        if (!keyDown) { keySeenUp_ = true; return false; }
        if (keySeenUp_ && time_ >= kSkipAfter) { state_ = State::Finished; return true; }
        return false;
    }

    State GetState() const { return state_; }
    double Time() const { return time_; }
    double Clock() const { return clock_; }
    double HoldOffset() const { return holdOffset_; }
    bool WorldReady() const { return worldReady_; }
    bool UsingAudioClock() const { return !audioDead_; }

private:
    State state_ = State::Idle;
    double clock_ = 0.0;
    double time_ = 0.0;
    double holdOffset_ = 0.0;
    double lastNow_ = 0.0;
    double lastAudio_ = -1.0;
    double lastAudioChange_ = 0.0;
    bool audioDead_ = false;
    bool worldReady_ = false;
    bool keySeenUp_ = false;
};

} // namespace skr::intro
