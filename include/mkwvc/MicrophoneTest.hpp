#pragma once

#include "mkwvc/AudioEngine.hpp"
#include "mkwvc/AudioProcessor.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

namespace mkwvc {

class MicrophoneTest {
public:
    MicrophoneTest(std::string captureDevice={},std::string playbackDevice={});
    ~MicrophoneTest();

    MicrophoneTest(const MicrophoneTest&)=delete;
    MicrophoneTest& operator=(const MicrophoneTest&)=delete;

    void start();
    void stop();
    void setCaptureDevice(std::string captureDevice);
    void setPlaybackDevice(std::string playbackDevice);
    void setMicrophoneGain(float gain);
    void setPlaybackVolume(float volume);
    void setMonitorEnabled(bool enabled);
    void setAudioProcessingSettings(const AudioProcessingSettings& settings);
    std::uint32_t micPeak() const;
    bool running() const;

private:
    void loop();

    AudioEngine audio_;
    AudioProcessor processor_;
    std::atomic<bool> running_{false};
    std::atomic<float> microphoneGain_{1.0f};
    std::atomic<float> playbackVolume_{1.0f};
    std::atomic<bool> monitorEnabled_{true};
    std::atomic<std::uint32_t> micPeak_{0};
    std::thread thread_;
};

}
