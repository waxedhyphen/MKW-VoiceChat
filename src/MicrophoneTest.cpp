#include "mkwvc/MicrophoneTest.hpp"
#include "mkwvc/VoiceFormat.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <span>
#include <thread>
#include <utility>

namespace mkwvc {

namespace {

std::uint32_t peakOf(std::span<const std::int16_t> samples) {
    std::uint32_t peak=0;
    for(const auto sample:samples) {
        const auto value=sample<0 ? static_cast<std::uint32_t>(-static_cast<std::int32_t>(sample)) : static_cast<std::uint32_t>(sample);
        peak=std::max(peak,value);
    }
    return peak;
}

constexpr float BaseMicrophoneGain=1.35f;
constexpr float BasePlaybackGain=3.375f;

float softLimit(float value) {
    constexpr float knee=30000.0f;
    constexpr float ceiling=32600.0f;
    const float magnitude=std::abs(value);
    if(magnitude<=knee) return value;
    const float compressed=knee+(ceiling-knee)*(1.0f-std::exp(-(magnitude-knee)/(ceiling-knee)));
    return std::copysign(compressed,value);
}

void applyGain(std::span<std::int16_t> samples,float gain,float& limiterGain) {
    gain=std::max(0.0f,gain);
    for(auto& sample:samples) {
        const float value=softLimit(static_cast<float>(sample)*gain);
        sample=static_cast<std::int16_t>(std::clamp(
            static_cast<std::int32_t>(std::lround(value)),-32768,32767));
    }
    limiterGain=1.0f;
}

}

MicrophoneTest::MicrophoneTest(std::string captureDevice,std::string playbackDevice):
    audio_(std::move(captureDevice),std::move(playbackDevice)) {}

MicrophoneTest::~MicrophoneTest(){stop();}

void MicrophoneTest::start() {
    if(running_.exchange(true)) return;
    try {
        audio_.start();
        thread_=std::thread(&MicrophoneTest::loop,this);
    } catch(...) {
        running_=false;
        audio_.stop();
        throw;
    }
}

void MicrophoneTest::stop() {
    running_=false;
    if(thread_.joinable()) thread_.join();
    audio_.stop();
    micPeak_=0;
}

void MicrophoneTest::setCaptureDevice(std::string captureDevice) {
    audio_.setCaptureDevice(std::move(captureDevice));
}

void MicrophoneTest::setPlaybackDevice(std::string playbackDevice) {
    audio_.setPlaybackDevice(std::move(playbackDevice));
}

void MicrophoneTest::setMicrophoneGain(float gain) {
    microphoneGain_.store(std::clamp(gain,0.25f,5.0f),std::memory_order_relaxed);
}

void MicrophoneTest::setPlaybackVolume(float volume) {
    playbackVolume_.store(std::clamp(volume,0.0f,3.0f),std::memory_order_relaxed);
}

void MicrophoneTest::setMonitorEnabled(bool enabled) {
    monitorEnabled_.store(enabled,std::memory_order_relaxed);
}

void MicrophoneTest::setAudioProcessingSettings(const AudioProcessingSettings& settings) {
    processor_.setSettings(settings);
}

std::uint32_t MicrophoneTest::micPeak() const {
    return micPeak_.load(std::memory_order_relaxed);
}

bool MicrophoneTest::running() const {
    return running_.load(std::memory_order_relaxed);
}

void MicrophoneTest::loop() {
    std::array<std::int16_t,VoiceFormat::FrameSamples> samples{};
    std::array<std::int16_t,VoiceFormat::FrameSamples> monitor{};
    std::size_t filled=0;
    float microphoneLimiterGain=1.0f;
    float monitorLimiterGain=1.0f;

    while(running_) {
        filled+=audio_.readCaptured(std::span<std::int16_t>(samples).subspan(filled));
        if(filled<VoiceFormat::FrameSamples) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        processor_.processCapture(samples);
        applyGain(
            samples,
            microphoneGain_.load(std::memory_order_relaxed)*BaseMicrophoneGain,
            microphoneLimiterGain);
        processor_.processPostGainSuppression(samples);
        micPeak_.store(peakOf(samples),std::memory_order_relaxed);

        if(monitorEnabled_.load(std::memory_order_relaxed)) {
            std::copy(samples.begin(),samples.end(),monitor.begin());
            applyGain(
                monitor,
                playbackVolume_.load(std::memory_order_relaxed)*BasePlaybackGain,
                monitorLimiterGain);
            audio_.queueMonitor(monitor);
        }
        filled=0;
    }
}

}
