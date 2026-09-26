#pragma once

#include "mkwvc/VoiceFormat.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace mkwvc {

class AudioEngine {
public:
    static constexpr std::uint32_t SampleRate=VoiceFormat::SampleRate;
    static constexpr std::uint32_t Channels=VoiceFormat::Channels;
    static constexpr std::size_t FrameSamples=VoiceFormat::FrameSamples;

    explicit AudioEngine(std::string captureDevice={},std::string playbackDevice={});
    ~AudioEngine();

    AudioEngine(const AudioEngine&)=delete;
    AudioEngine& operator=(const AudioEngine&)=delete;

    static std::vector<std::string> captureDevices();
    static std::vector<std::string> playbackDevices();

    void start();
    void stop();
    void setCaptureDevice(std::string captureDevice);
    void setPlaybackDevice(std::string playbackDevice);
    void setPlaybackMuted(bool muted);
    std::size_t readCaptured(std::span<std::int16_t> samples);
    std::size_t queuePlayback(std::span<const std::int16_t> samples);
    std::size_t queueMonitor(std::span<const std::int16_t> samples);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}
