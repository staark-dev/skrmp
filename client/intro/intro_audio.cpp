#include "intro/intro_audio.h"
#include "intro/intro_timeline.h"
#include "miniaudio.h"
#include <atomic>
#include <mutex>

namespace skr::intro {

namespace {
constexpr ma_uint32 kChannels = 2;
constexpr ma_uint32 kSampleRate = 48000;
}

struct IntroAudio::Impl {
    ma_decoder decoder{};
    ma_device device{};
    bool decoderOpen = false;
    bool deviceOpen = false;
    bool started = false;
    std::atomic<unsigned long long> framesDelivered{0};
    std::atomic<bool> worldReady{false};
    std::atomic<double> holdOffset{0.0};
    mutable std::mutex mutex;
};

static void DataCallback(ma_device* device, void* output, const void*, ma_uint32 frameCount) {
    auto* impl = static_cast<IntroAudio::Impl*>(device->pUserData);
    auto* out = static_cast<float*>(output);
    unsigned long long first = impl->framesDelivered.load(std::memory_order_relaxed);
    bool ready = impl->worldReady.load(std::memory_order_relaxed);
    double offset = impl->holdOffset.load(std::memory_order_relaxed);

    // Music loops while the intro holds for the world; silence once the fade-out ended.
    ma_uint64 read = 0;
    for (int pass = 0; pass < 2 && read < frameCount; ++pass) {
        ma_uint64 got = 0;
        ma_decoder_read_pcm_frames(&impl->decoder, out + read * kChannels, frameCount - read, &got);
        read += got;
        if (read < frameCount) ma_decoder_seek_to_pcm_frame(&impl->decoder, 0);
    }
    for (ma_uint64 i = read; i < frameCount; ++i)
        for (ma_uint32 c = 0; c < kChannels; ++c) out[i * kChannels + c] = 0.0f;

    for (ma_uint32 i = 0; i < frameCount; ++i) {
        double playback = static_cast<double>(first + i) / kSampleRate;
        double intro = playback - offset;
        if (!ready && intro > kHoldAt) intro = kHoldAt;
        float g = AudioGain(static_cast<float>(playback), static_cast<float>(intro));
        for (ma_uint32 c = 0; c < kChannels; ++c) out[i * kChannels + c] *= g;
    }
    impl->framesDelivered.store(first + frameCount, std::memory_order_relaxed);
}

void IntroAudio::SetHold(bool worldReady, double holdOffset) {
    impl_->holdOffset.store(holdOffset, std::memory_order_relaxed);
    impl_->worldReady.store(worldReady, std::memory_order_relaxed);
}

IntroAudio::IntroAudio() : impl_(std::make_unique<Impl>()) {}
IntroAudio::~IntroAudio() { Close(); }

bool IntroAudio::Open(const std::wstring& path, std::string& error) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    ma_decoder_config dc = ma_decoder_config_init(ma_format_f32, kChannels, kSampleRate);
    if (ma_decoder_init_file_w(path.c_str(), &dc, &impl_->decoder) != MA_SUCCESS) {
        error = "MP3 decoder init failed";
        return false;
    }
    impl_->decoderOpen = true;

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = kChannels;
    cfg.sampleRate = kSampleRate;
    cfg.dataCallback = DataCallback;
    cfg.pUserData = impl_.get();
    if (ma_device_init(nullptr, &cfg, &impl_->device) != MA_SUCCESS) {
        ma_decoder_uninit(&impl_->decoder);
        impl_->decoderOpen = false;
        error = "no audio output device";
        return false;
    }
    impl_->deviceOpen = true;
    return true;
}

void IntroAudio::Start() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->deviceOpen && !impl_->started && ma_device_start(&impl_->device) == MA_SUCCESS)
        impl_->started = true;
}

void IntroAudio::Pause() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->deviceOpen && impl_->started && ma_device_stop(&impl_->device) == MA_SUCCESS)
        impl_->started = false;
}

void IntroAudio::Resume() { Start(); }

double IntroAudio::Position() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->deviceOpen) return -1.0;
    return static_cast<double>(impl_->framesDelivered.load(std::memory_order_relaxed)) / kSampleRate;
}

void IntroAudio::Close() {
    if (!impl_) return;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->deviceOpen) { ma_device_uninit(&impl_->device); impl_->deviceOpen = false; }
    if (impl_->decoderOpen) { ma_decoder_uninit(&impl_->decoder); impl_->decoderOpen = false; }
    impl_->started = false;
}

} // namespace skr::intro
