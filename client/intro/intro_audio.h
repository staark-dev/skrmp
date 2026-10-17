#pragma once
#include <memory>
#include <string>

namespace skr::intro {

// Local MP3 playback for the intro through miniaudio 0.11.23 (deps/miniaudio-0.11.23).
// Position() counts frames handed to the device, so it runs ahead of what is heard by
// the device buffer latency (miniaudio's default period configuration, ~tens of ms);
// it has not been measured in game.
class IntroAudio {
public:
    IntroAudio();
    ~IntroAudio();

    // Decoder + device init. Slow (device enumeration); call from a worker thread.
    bool Open(const std::wstring& mp3Path, std::string& error);
    void Start();
    void Pause();
    void Resume();
    // Seconds of audio delivered to the device, or -1 when not open.
    double Position() const;
    // Mirrors Transport's hold so the fade-out follows the intro time, not the playback time.
    void SetHold(bool worldReady, double holdOffset);
    // Stops the device (joins its callback) and frees the decoder. Safe to call twice.
    void Close();

    struct Impl;  // defined in intro_audio.cpp; public so the C device callback can reach it

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace skr::intro
