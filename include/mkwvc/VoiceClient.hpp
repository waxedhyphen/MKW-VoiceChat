#pragma once

#include "mkwvc/AudioEngine.hpp"
#include "mkwvc/AudioProcessor.hpp"
#include "mkwvc/NetworkSimulator.hpp"
#include "mkwvc/OpusCodec.hpp"
#include "mkwvc/VoiceTransport.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace mkwvc {

struct PeerVoiceStats {
    std::string memberId;
    std::uint64_t txPackets=0;
    std::uint64_t rxPackets=0;
    std::uint64_t rawRxPackets=0;
    std::uint64_t protocolMismatchPackets=0;
    std::uint64_t txBytes=0;
    std::uint64_t rxBytes=0;
    std::uint64_t simulatedDrops=0;
    std::uint64_t simulatedDuplicates=0;
    std::uint64_t simulatedReorders=0;
    std::uint64_t reorderedRxPackets=0;
    std::uint64_t latePackets=0;
    std::uint64_t plcFrames=0;
    std::uint64_t fecAttempts=0;
    std::uint64_t decoderErrors=0;
    std::uint64_t sequenceResyncs=0;
    std::uint64_t streamRestarts=0;
    std::uint64_t jitterBufferExpansions=0;
    std::uint32_t simulationQueueDepth=0;
    std::uint32_t jitterBufferQueuedPackets=0;
    std::uint32_t jitterBufferTargetMs=0;
    std::uint32_t jitterBufferCurrentMs=0;
    float estimatedJitterMs=0.0f;
    float volume=1.0f;
    std::uint32_t voicePeak=0;
    bool speaking=false;
    bool remoteMuted=false;
    bool remoteDeafened=false;
    bool receiverRunning=false;
};

struct VoiceStats {
    std::uint64_t txPackets=0;
    std::uint64_t rxPackets=0;
    std::uint64_t rawRxPackets=0;
    std::uint64_t protocolMismatchPackets=0;
    std::uint64_t txBytes=0;
    std::uint64_t rxBytes=0;
    std::uint64_t simulatedDrops=0;
    std::uint64_t simulatedDuplicates=0;
    std::uint64_t simulatedReorders=0;
    std::uint64_t reorderedRxPackets=0;
    std::uint64_t latePackets=0;
    std::uint64_t plcFrames=0;
    std::uint64_t fecAttempts=0;
    std::uint64_t decoderErrors=0;
    std::uint64_t sequenceResyncs=0;
    std::uint64_t streamRestarts=0;
    std::uint64_t jitterBufferExpansions=0;
    std::uint32_t simulationQueueDepth=0;
    std::uint32_t jitterBufferQueuedPackets=0;
    std::uint32_t jitterBufferTargetMs=0;
    std::uint32_t jitterBufferCurrentMs=0;
    float estimatedJitterMs=0.0f;
    std::uint32_t micPeak=0;
    std::uint32_t playbackPeak=0;
    bool transmitting=false;
    bool running=false;
};

class VoiceClient {
public:
    VoiceClient(std::string peerAddress,std::uint16_t port,std::string captureDevice={},std::string playbackDevice={});
    VoiceClient(std::unique_ptr<VoiceTransport> transport,std::string captureDevice={},std::string playbackDevice={});
    VoiceClient(std::string memberId,std::unique_ptr<VoiceTransport> transport,std::string captureDevice={},std::string playbackDevice={});
    ~VoiceClient();

    VoiceClient(const VoiceClient&)=delete;
    VoiceClient& operator=(const VoiceClient&)=delete;

    void start();
    void stop();

    void addPeer(std::string memberId,std::unique_ptr<VoiceTransport> transport);
    void removePeer(const std::string& memberId);
    bool hasPeer(const std::string& memberId) const;
    std::size_t peerCount() const;
    std::vector<std::string> peerIds() const;

    void setCodecSettings(const OpusCodecSettings& settings);
    void setNetworkSimulation(const NetworkSimulationSettings& settings);
    void setCaptureDevice(std::string captureDevice);
    void setPlaybackDevice(std::string playbackDevice);
    void setMicrophoneGain(float gain);
    void setAudioProcessingSettings(const AudioProcessingSettings& settings);
    void setMicrophoneTestEnabled(bool enabled);
    void setPlaybackVolume(float volume);
    void setRemoteVolume(float volume);
    void setRemoteVolume(const std::string& memberId,float volume);
    void setTransmitEnabled(bool enabled);
    void setLocalStatus(bool muted,bool deafened);
    void setVoiceActivation(bool enabled,float threshold);
    void setDeafened(bool deafened);

    std::string localAddress() const;
    std::string localAddress(const std::string& memberId) const;
    VoiceStats stats() const;
    std::vector<PeerVoiceStats> peerStats() const;

private:
    struct PeerState;

    void senderLoop();
    void receiverLoop(const std::shared_ptr<PeerState>& peer);
    void mixerLoop();
    void startPeerReceiver(const std::shared_ptr<PeerState>& peer);
    std::vector<std::shared_ptr<PeerState>> peerSnapshot() const;
    OpusCodecSettings codecSettings() const;
    NetworkSimulationSettings networkSimulation() const;

    AudioEngine audio_;
    AudioProcessor processor_;
    OpusEncoderCodec encoder_;

    mutable std::mutex peersMutex_;
    std::unordered_map<std::string,std::shared_ptr<PeerState>> peers_;

    mutable std::mutex settingsMutex_;
    OpusCodecSettings codecSettings_{};
    NetworkSimulationSettings networkSimulation_{};

    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> txPackets_{0};
    std::atomic<std::uint64_t> rxPackets_{0};
    std::atomic<std::uint64_t> rawRxPackets_{0};
    std::atomic<std::uint64_t> protocolMismatchPackets_{0};
    std::atomic<std::uint64_t> txBytes_{0};
    std::atomic<std::uint64_t> rxBytes_{0};
    std::atomic<std::uint64_t> simulatedDrops_{0};
    std::atomic<std::uint64_t> simulatedDuplicates_{0};
    std::atomic<std::uint64_t> simulatedReorders_{0};
    std::atomic<std::uint64_t> reorderedRxPackets_{0};
    std::atomic<std::uint64_t> latePackets_{0};
    std::atomic<std::uint64_t> plcFrames_{0};
    std::atomic<std::uint64_t> fecAttempts_{0};
    std::atomic<std::uint64_t> decoderErrors_{0};
    std::atomic<std::uint64_t> sequenceResyncs_{0};
    std::atomic<std::uint64_t> streamRestarts_{0};
    std::atomic<std::uint64_t> jitterBufferExpansions_{0};
    std::atomic<std::uint32_t> simulationQueueDepth_{0};
    std::atomic<std::uint32_t> jitterBufferQueuedPackets_{0};
    std::atomic<std::uint32_t> jitterBufferTargetMs_{0};
    std::atomic<std::uint32_t> jitterBufferCurrentMs_{0};
    std::atomic<std::uint32_t> estimatedJitterMicros_{0};
    std::atomic<std::uint32_t> micPeak_{0};
    std::atomic<std::uint32_t> playbackPeak_{0};
    std::atomic<float> microphoneGain_{1.0f};
    std::atomic<float> playbackVolume_{1.0f};
    std::atomic<bool> transmitEnabled_{true};
    std::atomic<bool> localMutedStatus_{false};
    std::atomic<bool> localDeafenedStatus_{false};
    std::atomic<bool> voiceActivationEnabled_{false};
    std::atomic<float> voiceActivationThreshold_{0.04f};
    std::atomic<bool> transmitting_{false};
    std::atomic<bool> deafened_{false};
    std::atomic<bool> microphoneTestEnabled_{false};

    std::thread sender_;
    std::thread mixer_;
};

}
