#include "mkwvc/AudioEngine.hpp"
#include "mkwvc/SpscRingBuffer.hpp"

#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <exception>
#include <stdexcept>
#include <unordered_map>

namespace mkwvc {

namespace {

std::vector<std::string> numberedDeviceNames(
    const ma_device_info* infos,
    ma_uint32 count) {
    std::unordered_map<std::string,std::size_t> totals;
    for(ma_uint32 i=0;i<count;++i) ++totals[infos[i].name];

    std::unordered_map<std::string,std::size_t> seen;
    std::vector<std::string> result;
    result.reserve(count);
    for(ma_uint32 i=0;i<count;++i) {
        const std::string raw=infos[i].name;
        const auto occurrence=++seen[raw];
        if(totals[raw]>1) {
            result.push_back(raw+" ["+std::to_string(occurrence)+"]");
        } else {
            result.push_back(raw);
        }
    }
    return result;
}

const ma_device_id* resolveDeviceId(
    const std::string& selected,
    ma_device_info* infos,
    ma_uint32 count) {
    if(selected.empty()) return nullptr;

    const auto labels=numberedDeviceNames(infos,count);
    for(ma_uint32 i=0;i<count;++i) {
        if(selected==labels[i]) return &infos[i].id;
    }

    for(ma_uint32 i=0;i<count;++i) {
        if(selected==infos[i].name) return &infos[i].id;
    }
    return nullptr;
}

}

class AudioEngine::Impl {
public:
    explicit Impl(const std::string& captureDevice,const std::string& playbackDevice) {
        if(ma_context_init(nullptr,0,nullptr,&context_)!=MA_SUCCESS) throw std::runtime_error("Failed to initialize audio context");
        contextInitialized_=true;

        try {
            initializeDevice(captureDevice,playbackDevice);
            captureDevice_=captureDevice;
            playbackDevice_=playbackDevice;
        } catch(...) {
            ma_context_uninit(&context_);
            contextInitialized_=false;
            throw;
        }
    }

    ~Impl() {
        stop();
        if(deviceInitialized_) ma_device_uninit(&device_);
        if(contextInitialized_) ma_context_uninit(&context_);
    }

    void start() {
        if(running_) return;
        playbackPrimed_=false;
        monitorPrimed_=false;
        if(ma_device_start(&device_)!=MA_SUCCESS) throw std::runtime_error("Failed to start audio device");
        running_=true;
    }

    void stop() {
        if(!running_) return;
        ma_device_stop(&device_);
        running_=false;
    }

    void setCaptureDevice(const std::string& captureDevice) {
        changeDevices(captureDevice,playbackDevice_);
    }

    void setPlaybackDevice(const std::string& playbackDevice) {
        changeDevices(captureDevice_,playbackDevice);
    }

    void setPlaybackMuted(bool muted) {
        playbackMuted_.store(muted,std::memory_order_relaxed);
    }

    std::size_t readCaptured(std::span<std::int16_t> samples) { return capture_.pop(samples); }
    std::size_t queuePlayback(std::span<const std::int16_t> samples) { return playback_.push(samples); }
    std::size_t queueMonitor(std::span<const std::int16_t> samples) { return monitor_.push(samples); }

private:
    void changeDevices(const std::string& captureDevice,const std::string& playbackDevice) {
        if(captureDevice==captureDevice_ && playbackDevice==playbackDevice_) return;

        const auto previousCapture=captureDevice_;
        const auto previousPlayback=playbackDevice_;
        const bool wasRunning=running_;
        if(wasRunning) stop();
        if(deviceInitialized_) {
            ma_device_uninit(&device_);
            deviceInitialized_=false;
            device_={};
        }

        try {
            initializeDevice(captureDevice,playbackDevice);
            captureDevice_=captureDevice;
            playbackDevice_=playbackDevice;
            if(wasRunning) start();
        } catch(...) {
            const auto error=std::current_exception();
            if(running_) stop();
            if(deviceInitialized_) {
                ma_device_uninit(&device_);
                deviceInitialized_=false;
                device_={};
            }

            try {
                initializeDevice(previousCapture,previousPlayback);
                captureDevice_=previousCapture;
                playbackDevice_=previousPlayback;
                if(wasRunning) start();
            } catch(...) {}

            std::rethrow_exception(error);
        }
    }

    void initializeDevice(const std::string& captureDevice,const std::string& playbackDevice) {
        auto config=ma_device_config_init(ma_device_type_duplex);
        config.capture.format=ma_format_s16;
        config.capture.channels=AudioEngine::Channels;
        config.playback.format=ma_format_s16;
        config.playback.channels=AudioEngine::Channels;
        config.sampleRate=AudioEngine::SampleRate;
        config.dataCallback=audioCallback;
        config.pUserData=this;

        ma_device_info* playbackInfos=nullptr;
        ma_uint32 playbackCount=0;
        ma_device_info* captureInfos=nullptr;
        ma_uint32 captureCount=0;

        if(!captureDevice.empty() || !playbackDevice.empty()) {
            if(ma_context_get_devices(&context_,&playbackInfos,&playbackCount,&captureInfos,&captureCount)!=MA_SUCCESS) {
                throw std::runtime_error("Failed to enumerate audio devices");
            }

            if(!captureDevice.empty()) {
                const ma_device_id* selectedId=resolveDeviceId(
                    captureDevice,captureInfos,captureCount);
                if(!selectedId) throw std::runtime_error("Selected input device is no longer available");
                config.capture.pDeviceID=selectedId;
            }

            if(!playbackDevice.empty()) {
                const ma_device_id* selectedId=resolveDeviceId(
                    playbackDevice,playbackInfos,playbackCount);
                if(!selectedId) throw std::runtime_error("Selected output device is no longer available");
                config.playback.pDeviceID=selectedId;
            }
        }

        if(ma_device_init(&context_,&config,&device_)!=MA_SUCCESS) throw std::runtime_error("Failed to initialize audio device");
        deviceInitialized_=true;
    }

    static void audioCallback(ma_device* device,void* output,const void* input,ma_uint32 frameCount) {
        static_cast<Impl*>(device->pUserData)->process(static_cast<std::int16_t*>(output),static_cast<const std::int16_t*>(input),static_cast<std::size_t>(frameCount));
    }

    void process(std::int16_t* output,const std::int16_t* input,std::size_t frameCount) {
        if(input) capture_.push(std::span<const std::int16_t>(input,frameCount));
        if(!output) return;

        std::fill(output,output+frameCount,0);

        if(!playbackPrimed_ &&
           playback_.available()>=PlaybackPrebufferSamples) {
            playbackPrimed_=true;
        }
        if(playbackPrimed_) {
            const auto read=playback_.pop(std::span<std::int16_t>(output,frameCount));
            if(read<frameCount) {
                std::fill(output+read,output+frameCount,0);
                playbackPrimed_=false;
            }
        }

        if(playbackMuted_.load(std::memory_order_relaxed)) {
            std::fill(output,output+frameCount,0);
        }

        if(!monitorPrimed_ &&
           monitor_.available()>=MonitorPrebufferSamples) {
            monitorPrimed_=true;
        }

        if(monitorPrimed_) {
            std::array<std::int16_t,1024> monitorSamples{};
            std::size_t offset=0;
            while(offset<frameCount) {
                const auto count=std::min<std::size_t>(monitorSamples.size(),frameCount-offset);
                const auto monitorRead=monitor_.pop(std::span<std::int16_t>(monitorSamples.data(),count));
                for(std::size_t i=0;i<monitorRead;++i) {
                    const auto mixed=
                        static_cast<std::int32_t>(output[offset+i])+
                        static_cast<std::int32_t>(monitorSamples[i]);
                    output[offset+i]=static_cast<std::int16_t>(
                        std::clamp(mixed,-32768,32767));
                }
                if(monitorRead<count) {
                    monitorPrimed_=false;
                    break;
                }
                offset+=count;
            }
        }
    }

    static constexpr std::size_t BufferSamples=AudioEngine::SampleRate*2+1;
    static constexpr std::size_t PlaybackPrebufferSamples=
        AudioEngine::SampleRate*60/1000;
    static constexpr std::size_t MonitorPrebufferSamples=
        AudioEngine::SampleRate*40/1000;
    SpscRingBuffer<std::int16_t,BufferSamples> capture_;
    SpscRingBuffer<std::int16_t,BufferSamples> playback_;
    SpscRingBuffer<std::int16_t,BufferSamples> monitor_;
    ma_context context_{};
    ma_device device_{};
    std::string captureDevice_;
    std::string playbackDevice_;
    std::atomic<bool> playbackMuted_{false};
    bool playbackPrimed_=false;
    bool monitorPrimed_=false;
    bool contextInitialized_=false;
    bool deviceInitialized_=false;
    bool running_=false;
};

AudioEngine::AudioEngine(std::string captureDevice,std::string playbackDevice):
    impl_(std::make_unique<Impl>(captureDevice,playbackDevice)) {}
AudioEngine::~AudioEngine()=default;

std::vector<std::string> AudioEngine::captureDevices() {
    ma_context context{};
    if(ma_context_init(nullptr,0,nullptr,&context)!=MA_SUCCESS) return {};

    ma_device_info* playbackInfos=nullptr;
    ma_uint32 playbackCount=0;
    ma_device_info* captureInfos=nullptr;
    ma_uint32 captureCount=0;
    std::vector<std::string> devices;

    if(ma_context_get_devices(&context,&playbackInfos,&playbackCount,&captureInfos,&captureCount)==MA_SUCCESS) {
        devices=numberedDeviceNames(captureInfos,captureCount);
    }

    ma_context_uninit(&context);
    return devices;
}

std::vector<std::string> AudioEngine::playbackDevices() {
    ma_context context{};
    if(ma_context_init(nullptr,0,nullptr,&context)!=MA_SUCCESS) return {};

    ma_device_info* playbackInfos=nullptr;
    ma_uint32 playbackCount=0;
    ma_device_info* captureInfos=nullptr;
    ma_uint32 captureCount=0;
    std::vector<std::string> devices;

    if(ma_context_get_devices(&context,&playbackInfos,&playbackCount,&captureInfos,&captureCount)==MA_SUCCESS) {
        devices=numberedDeviceNames(playbackInfos,playbackCount);
    }

    ma_context_uninit(&context);
    return devices;
}

void AudioEngine::start(){impl_->start();}
void AudioEngine::stop(){impl_->stop();}
void AudioEngine::setCaptureDevice(std::string captureDevice){impl_->setCaptureDevice(captureDevice);}
void AudioEngine::setPlaybackDevice(std::string playbackDevice){impl_->setPlaybackDevice(playbackDevice);}
void AudioEngine::setPlaybackMuted(bool muted){impl_->setPlaybackMuted(muted);}
std::size_t AudioEngine::readCaptured(std::span<std::int16_t> samples){return impl_->readCaptured(samples);}
std::size_t AudioEngine::queuePlayback(std::span<const std::int16_t> samples){return impl_->queuePlayback(samples);}
std::size_t AudioEngine::queueMonitor(std::span<const std::int16_t> samples){return impl_->queueMonitor(samples);}

}
