#include "mkwvc/AppConfig.hpp"
#include "mkwvc/AudioEngine.hpp"
#include "mkwvc/MicrophoneTest.hpp"
#include "mkwvc/LoadTest.hpp"
#include "mkwvc/VoiceClient.hpp"
#include "mkwvc/VoiceFormat.hpp"

#ifdef MKWVC_HAS_ICE
#include "mkwvc/IcePeerTransport.hpp"
#include "mkwvc/IceSignal.hpp"
#include "mkwvc/SignalingClient.hpp"
#endif

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <iostream>
#include <iterator>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace {

struct PeerUiInfo {
    std::string memberId;
    std::string name;
    std::string country;
    float volume=1.0f;
};

struct RetroRewindPlayerInfo {
    std::string participantId;
    std::string name;
    bool voiceChat=false;
};

#ifdef MKWVC_HAS_ICE
struct RoomPeerLink {
    std::string memberId;
    std::unique_ptr<mkwvc::IcePeerTransport> setup;
    mkwvc::IcePeerTransport* transport=nullptr;
    std::optional<mkwvc::IceDescriptionSignal> localDescription;
    std::vector<mkwvc::IceCandidateSignal> localCandidates;
    std::string localSignal;
    bool signalSent=false;
    bool remoteApplied=false;
    bool offerer=false;
};
#endif

struct UiState {
    std::array<char,64> peer{};
    int port=50000;
    std::array<char,64> playerName{};
    std::vector<std::string> inputDevices;
    int inputDeviceIndex=0;
    std::vector<std::string> outputDevices;
    int outputDeviceIndex=0;
    float microphoneGain=1.0f;
    float playbackVolume=1.0f;
    mkwvc::AudioProcessingSettings audioProcessing{};
    bool microphoneMuted=false;
    bool deafened=false;
    bool pushToTalk=false;
    bool microphoneTest=false;
    bool microphoneTestRestoreDeafened=false;
    std::unique_ptr<mkwvc::MicrophoneTest> localMicrophoneTest;
    mkwvc::OpusCodecSettings codecSettings{};
    mkwvc::NetworkSimulationSettings networkSimulation{};
    std::unique_ptr<mkwvc::VoiceClient> client;
    std::string localAddress;
    std::string error;
    std::uint64_t lastTx=0;
    std::uint64_t lastRx=0;
    std::uint64_t lastTxBytes=0;
    std::uint64_t lastRxBytes=0;
    std::uint64_t txRate=0;
    std::uint64_t rxRate=0;
    std::uint64_t txByteRate=0;
    std::uint64_t rxByteRate=0;
    std::uint64_t observedRxPackets=0;
    bool peerSeen=false;
    std::chrono::steady_clock::time_point lastPeerPacket=std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point lastRateUpdate=std::chrono::steady_clock::now();

#ifdef MKWVC_HAS_ICE
    int connectionMode=0;
    int iceRole=0;
    std::unique_ptr<mkwvc::IcePeerTransport> iceSetup;
    mkwvc::IcePeerTransport* iceTransport=nullptr;
    std::optional<mkwvc::IceDescriptionSignal> localIceDescription;
    std::vector<mkwvc::IceCandidateSignal> localIceCandidates;
    std::string localIceSignal;
    std::array<char,32768> remoteIceSignal{};
    bool remoteIceApplied=false;
    std::array<char,256> signalingServer{};
    std::vector<std::string> iceServers{"stun:stun.l.google.com:19302"};
    bool forceRelay=false;
    std::array<char,16> roomCodeInput{};
    std::string activeRoomCode;
    std::vector<PeerUiInfo> roomPeers;
    std::vector<RoomPeerLink> roomPeerLinks;
    std::unique_ptr<mkwvc::SignalingClient> signaling;
    bool roomPeerReady=false;
    bool roomSignalSent=false;
    bool memberAddressedSignaling=false;
    std::string activeVoicePeerId;
    bool roomIceServersReady=false;
    bool roomRequestPending=false;
    bool roomStateQueued=false;
    int queuedRoomState=0;
    std::string queuedRoomCode;
    std::chrono::steady_clock::time_point roomStateDeadline{};
    int inFlightRoomState=0;
    std::string inFlightRoomCode;
    std::chrono::steady_clock::time_point roomRequestSentAt{};
    int roomRequestRetries=0;
    bool signalingReconnectQueued=false;
    int signalingReconnectRoomState=0;
    std::string signalingReconnectRoomCode;
    std::chrono::steady_clock::time_point signalingReconnectDeadline{};
    int signalingReconnectAttempts=0;
    bool signalingReconnectBlocked=false;
    std::string signalingMemberId;
    bool signalingReconnectOpenPending=false;
    int signalingReconnectOpenRoomState=0;
    std::string signalingReconnectOpenRoomCode;
    std::chrono::steady_clock::time_point signalingReconnectOpenDeadline{};
    std::string signalingDebug;

    std::array<char,16> rrProfileId{};
    std::array<char,16> rrSessionKey{};
    std::array<char,40> rrGameName{};
    bool rrAuthPending=false;
    bool rrAuthenticated=false;
    std::string rrAuthStatus="Not authenticated";
    std::string rrVerifiedProfileId;
    std::string rrRoomId;
    std::string rrRoomInstanceId;
    std::string rrRoomCreated;
    std::vector<RetroRewindPlayerInfo> rrPlayers;
    std::chrono::steady_clock::time_point rrLastSync{};
#endif
};

std::string makeSignalingMemberId() {
    constexpr char hex[]="0123456789ABCDEF";
    std::random_device random;
    std::string id(32,'0');
    for(char& ch:id) ch=hex[random()&0x0F];
    return id;
}

float normalizePeak(std::uint32_t peak) {
    return std::clamp(static_cast<float>(peak)/32768.0f,0.0f,1.0f);
}

std::string selectedInputDevice(const UiState& state) {
    if(state.inputDeviceIndex<=0 || state.inputDeviceIndex>static_cast<int>(state.inputDevices.size())) return {};
    return state.inputDevices[static_cast<std::size_t>(state.inputDeviceIndex-1)];
}

std::string selectedOutputDevice(const UiState& state) {
    if(state.outputDeviceIndex<=0 || state.outputDeviceIndex>static_cast<int>(state.outputDevices.size())) return {};
    return state.outputDevices[static_cast<std::size_t>(state.outputDeviceIndex-1)];
}

mkwvc::AppConfig currentConfig(const UiState& state) {
    return {
        state.peer.data(),
        state.port,
        state.playerName.data(),
        selectedInputDevice(state),
        selectedOutputDevice(state),
        state.microphoneGain,
        state.playbackVolume,
        state.audioProcessing.normalization,
        state.audioProcessing.noiseSuppression,
        state.audioProcessing.noiseSuppressionStrength,
        state.signalingServer.data()
    };
}

void saveState(UiState& state) {
    try {
        mkwvc::saveConfig(currentConfig(state));
    } catch(const std::exception& error) {
        state.error=error.what();
    }
}

void loadState(UiState& state) {
    const auto config=mkwvc::loadConfig();
#ifdef MKWVC_HAS_ICE
    state.signalingMemberId=makeSignalingMemberId();
#endif
    std::snprintf(state.peer.data(),state.peer.size(),"%s",config.peerAddress.c_str());
    std::snprintf(state.playerName.data(),state.playerName.size(),"%s",config.playerName.c_str());
    state.port=config.port;
    state.microphoneGain=config.microphoneGain;
    state.playbackVolume=config.playbackVolume;
    state.audioProcessing.normalization=config.automaticNormalization;
    state.audioProcessing.noiseSuppression=config.noiseSuppression;
    state.audioProcessing.noiseSuppressionStrength=config.noiseSuppressionStrength;
    std::snprintf(state.signalingServer.data(),state.signalingServer.size(),"%s",config.signalingServer.c_str());
#ifdef MKWVC_HAS_ICE
    std::snprintf(state.rrGameName.data(),state.rrGameName.size(),"%s","mariokartwii");
#endif
    state.inputDevices=mkwvc::AudioEngine::captureDevices();
    state.outputDevices=mkwvc::AudioEngine::playbackDevices();
    state.inputDeviceIndex=0;
    state.outputDeviceIndex=0;

    if(!config.inputDevice.empty()) {
        for(std::size_t i=0;i<state.inputDevices.size();++i) {
            if(state.inputDevices[i]==config.inputDevice) {
                state.inputDeviceIndex=static_cast<int>(i+1);
                break;
            }
        }
    }

    if(!config.outputDevice.empty()) {
        for(std::size_t i=0;i<state.outputDevices.size();++i) {
            if(state.outputDevices[i]==config.outputDevice) {
                state.outputDeviceIndex=static_cast<int>(i+1);
                break;
            }
        }
    }
}

void refreshInputDevices(UiState& state) {
    const auto current=selectedInputDevice(state);
    state.inputDevices=mkwvc::AudioEngine::captureDevices();
    state.inputDeviceIndex=0;

    if(!current.empty()) {
        for(std::size_t i=0;i<state.inputDevices.size();++i) {
            if(state.inputDevices[i]==current) {
                state.inputDeviceIndex=static_cast<int>(i+1);
                break;
            }
        }
    }
}

void refreshOutputDevices(UiState& state) {
    const auto current=selectedOutputDevice(state);
    state.outputDevices=mkwvc::AudioEngine::playbackDevices();
    state.outputDeviceIndex=0;

    if(!current.empty()) {
        for(std::size_t i=0;i<state.outputDevices.size();++i) {
            if(state.outputDevices[i]==current) {
                state.outputDeviceIndex=static_cast<int>(i+1);
                break;
            }
        }
    }
}

bool pushToTalkHeld(const UiState& state) {
    return
        !state.pushToTalk ||
        (ImGui::IsKeyDown(ImGuiKey_V) && !ImGui::GetIO().WantTextInput);
}

void applyAudioProcessingSettings(UiState& state) {
    if(state.client) state.client->setAudioProcessingSettings(state.audioProcessing);
    if(state.localMicrophoneTest) state.localMicrophoneTest->setAudioProcessingSettings(state.audioProcessing);
}

void applyMicrophoneGain(UiState& state) {
    if(state.client) state.client->setMicrophoneGain(state.microphoneGain);
    if(state.localMicrophoneTest) state.localMicrophoneTest->setMicrophoneGain(state.microphoneGain);
}

void applyPlaybackVolume(UiState& state) {
    if(state.client) state.client->setPlaybackVolume(state.playbackVolume);
    if(state.localMicrophoneTest) state.localMicrophoneTest->setPlaybackVolume(state.playbackVolume);
}

void stopLocalMicrophoneTest(UiState& state) {
    if(!state.localMicrophoneTest) return;
    state.localMicrophoneTest->stop();
    state.localMicrophoneTest.reset();
}

bool startLocalMicrophoneTest(UiState& state) {
    if(state.localMicrophoneTest) return true;

    try {
        auto test=std::make_unique<mkwvc::MicrophoneTest>(selectedInputDevice(state),selectedOutputDevice(state));
        test->setAudioProcessingSettings(state.audioProcessing);
        test->setMicrophoneGain(state.microphoneGain);
        test->setPlaybackVolume(state.playbackVolume);
        test->setMonitorEnabled(pushToTalkHeld(state));
        test->start();
        state.localMicrophoneTest=std::move(test);
        state.error.clear();
        return true;
    } catch(const std::exception& error) {
        state.error=error.what();
        return false;
    }
}

void startMicrophoneTest(UiState& state) {
    if(state.microphoneTest) return;

    state.microphoneTestRestoreDeafened=state.deafened;
    state.deafened=true;
    state.microphoneTest=true;

    if(state.client) {
        state.client->setMicrophoneTestEnabled(pushToTalkHeld(state));
        state.client->setDeafened(true);
        state.client->setTransmitEnabled(false);
        return;
    }

    if(!startLocalMicrophoneTest(state)) {
        state.microphoneTest=false;
        state.deafened=state.microphoneTestRestoreDeafened;
    }
}

void stopMicrophoneTest(UiState& state) {
    if(!state.microphoneTest) return;

    if(state.client) state.client->setMicrophoneTestEnabled(false);
    stopLocalMicrophoneTest(state);
    state.microphoneTest=false;
    state.deafened=state.microphoneTestRestoreDeafened;

    if(state.client) state.client->setDeafened(state.deafened);
}

void changeInputDevice(UiState& state,int index) {
    const auto previous=state.inputDeviceIndex;
    state.inputDeviceIndex=index;

    if(state.client) {
        try {
            state.client->setCaptureDevice(selectedInputDevice(state));
            state.error.clear();
        } catch(const std::exception& error) {
            state.inputDeviceIndex=previous;
            state.error=error.what();
            return;
        }
    } else if(state.localMicrophoneTest) {
        try {
            state.localMicrophoneTest->setCaptureDevice(selectedInputDevice(state));
            state.error.clear();
        } catch(const std::exception& error) {
            state.inputDeviceIndex=previous;
            state.error=error.what();
            return;
        }
    }

    saveState(state);
}

void changeOutputDevice(UiState& state,int index) {
    const auto previous=state.outputDeviceIndex;
    state.outputDeviceIndex=index;

    if(state.client) {
        try {
            state.client->setPlaybackDevice(selectedOutputDevice(state));
            state.error.clear();
        } catch(const std::exception& error) {
            state.outputDeviceIndex=previous;
            state.error=error.what();
            return;
        }
    } else if(state.localMicrophoneTest) {
        try {
            state.localMicrophoneTest->setPlaybackDevice(selectedOutputDevice(state));
            state.error.clear();
        } catch(const std::exception& error) {
            state.outputDeviceIndex=previous;
            state.error=error.what();
            return;
        }
    }

    saveState(state);
}

void resetRates(UiState& state) {
    state.lastTx=0;
    state.lastRx=0;
    state.lastTxBytes=0;
    state.lastRxBytes=0;
    state.txRate=0;
    state.rxRate=0;
    state.txByteRate=0;
    state.rxByteRate=0;
    state.observedRxPackets=0;
    state.peerSeen=false;
    state.lastRateUpdate=std::chrono::steady_clock::now();
}

void stopClient(UiState& state) {
#ifdef MKWVC_HAS_ICE
    state.iceTransport=nullptr;
#endif
    if(state.client) {
        state.client->stop();
        state.client.reset();
    }
    resetRates(state);
}

#ifdef MKWVC_HAS_ICE
void resetIceTransport(UiState& state) {
    stopClient(state);
    state.roomPeerLinks.clear();
    state.iceSetup.reset();
    state.localIceDescription.reset();
    state.localIceCandidates.clear();
    state.localIceSignal.clear();
    state.remoteIceSignal.fill('\0');
    state.remoteIceApplied=false;
    state.roomSignalSent=false;
    state.localAddress.clear();
}

void clearRoomMembership(UiState& state) {
    resetIceTransport(state);
    state.activeRoomCode.clear();
    state.roomPeers.clear();
    state.roomPeerReady=false;
    state.memberAddressedSignaling=false;
    state.activeVoicePeerId.clear();
    state.roomIceServersReady=false;
    state.roomRequestPending=false;
    state.iceServers.clear();
}

void resetRoomQueue(UiState& state) {
    state.roomStateQueued=false;
    state.queuedRoomState=0;
    state.queuedRoomCode.clear();
    state.roomStateDeadline={};
}

void resetRoomRequest(UiState& state) {
    state.roomRequestPending=false;
    state.inFlightRoomState=0;
    state.inFlightRoomCode.clear();
    state.roomRequestSentAt={};
}

void resetSignalingReconnect(UiState& state) {
    state.signalingReconnectQueued=false;
    state.signalingReconnectRoomState=0;
    state.signalingReconnectRoomCode.clear();
    state.signalingReconnectDeadline={};
}

void resetSignalingOpenWait(UiState& state) {
    state.signalingReconnectOpenPending=false;
    state.signalingReconnectOpenRoomState=0;
    state.signalingReconnectOpenRoomCode.clear();
    state.signalingReconnectOpenDeadline={};
}

std::pair<int,std::string> desiredReconnectState(const UiState& state) {
    if(state.roomStateQueued && state.queuedRoomState!=0) {
        return {state.queuedRoomState,state.queuedRoomCode};
    }

    if(state.roomRequestPending && state.inFlightRoomState!=0) {
        return {state.inFlightRoomState,state.inFlightRoomCode};
    }

    if(!state.activeRoomCode.empty()) {
        return {3,state.activeRoomCode};
    }

    return {0,{}};
}

bool ensureSignaling(UiState& state) {
    if(state.signaling) return true;

    try {
        state.signaling=std::make_unique<mkwvc::SignalingClient>(std::string(state.signalingServer.data()));
        return true;
    } catch(const std::exception& error) {
        state.error=error.what();
        state.signaling.reset();
        return false;
    }
}

std::chrono::milliseconds reconnectDelay(int attempt) {
    const int shift=std::clamp(attempt-1,0,2);
    const auto base=std::chrono::seconds(1<<shift);
    const auto ticks=std::chrono::steady_clock::now().time_since_epoch().count();
    const auto jitter=std::chrono::milliseconds(static_cast<int>(ticks%251));
    return std::chrono::duration_cast<std::chrono::milliseconds>(base)+jitter;
}

void scheduleSignalingReconnect(UiState& state,int target,std::string code,std::string reason) {
    constexpr int maxReconnectAttempts=3;

    if(state.signalingReconnectBlocked || state.signalingReconnectQueued) return;

    resetRoomQueue(state);
    resetRoomRequest(state);
    resetSignalingOpenWait(state);
    state.signaling.reset();

    if(state.signalingReconnectAttempts>=maxReconnectAttempts) {
        const std::string retryRoomCode=
            !code.empty() ? code : state.activeRoomCode;
        const bool hadRoom=
            !state.activeRoomCode.empty() ||
            target==3;

        resetSignalingReconnect(state);
        state.signalingReconnectBlocked=true;

        if(hadRoom) {
            clearRoomMembership(state);
            state.roomCodeInput.fill('\0');

            const auto copyCount=std::min(
                retryRoomCode.size(),
                state.roomCodeInput.size()-1
            );
            std::copy_n(
                retryRoomCode.begin(),
                copyCount,
                state.roomCodeInput.begin()
            );
        }

        state.signalingDebug="Reconnect failed after 3/3 attempts.";
        state.error=
            std::move(reason)+
            (hadRoom
                ? " Automatic reconnect stopped. Local room state cleared."
                : " Automatic reconnect stopped. Try again.");
        return;
    }

    ++state.signalingReconnectAttempts;
    const auto delay=reconnectDelay(state.signalingReconnectAttempts);
    state.signalingReconnectQueued=true;
    state.signalingReconnectRoomState=target;
    state.signalingReconnectRoomCode=std::move(code);
    state.signalingReconnectDeadline=std::chrono::steady_clock::now()+delay;
    state.roomRequestPending=target!=0;
    state.signalingDebug=
        "Reconnect attempt "+std::to_string(state.signalingReconnectAttempts)+
        "/3 scheduled in "+std::to_string(delay.count())+" ms";
    state.error=std::move(reason)+" Retrying signaling in "+std::to_string(delay.count())+" ms...";
}

void processSignalingReconnect(UiState& state) {
    if(!state.signalingReconnectQueued || std::chrono::steady_clock::now()<state.signalingReconnectDeadline) return;

    const int target=state.signalingReconnectRoomState;
    std::string code=std::move(state.signalingReconnectRoomCode);
    resetSignalingReconnect(state);

    const int attempt=state.signalingReconnectAttempts;
    state.signalingDebug=
        "Reconnect attempt "+std::to_string(attempt)+"/3 opening WebSocket...";

    state.signalingReconnectOpenPending=true;
    state.signalingReconnectOpenRoomState=target;
    state.signalingReconnectOpenRoomCode=code;
    state.signalingReconnectOpenDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);

    if(!ensureSignaling(state)) {
        resetSignalingOpenWait(state);
        scheduleSignalingReconnect(state,target,std::move(code),"Failed to reconnect signaling.");
        return;
    }

    if(target==0) {
        state.roomRequestPending=false;
        state.error="Reconnecting signaling...";
        return;
    }

    state.roomStateQueued=true;
    state.roomStateDeadline=std::chrono::steady_clock::now();
    state.queuedRoomState=target;
    state.queuedRoomCode=std::move(code);
    state.roomRequestPending=true;
    state.error="Reconnecting signaling...";
}

void recoverTimedOutSignalingOpen(UiState& state) {
    if(!state.signalingReconnectOpenPending) return;
    if(state.signalingReconnectOpenDeadline==std::chrono::steady_clock::time_point{}) return;
    if(std::chrono::steady_clock::now()<state.signalingReconnectOpenDeadline) return;

    const int target=state.signalingReconnectOpenRoomState;
    std::string code=std::move(state.signalingReconnectOpenRoomCode);
    resetSignalingOpenWait(state);
    scheduleSignalingReconnect(state,target,std::move(code),"Signaling WebSocket open timed out.");
}

void resetIce(UiState& state) {
    clearRoomMembership(state);
    resetRoomQueue(state);
    resetRoomRequest(state);
    state.roomRequestRetries=0;
    resetSignalingReconnect(state);
    resetSignalingOpenWait(state);
    state.signalingReconnectAttempts=0;
    state.signalingReconnectBlocked=false;
    state.signalingDebug.clear();
    state.signaling.reset();
    state.iceServers={"stun:stun.l.google.com:19302"};
    state.roomCodeInput.fill('\0');
}

void queueRoomState(UiState& state,int target,std::string code={}) {
    resetSignalingReconnect(state);
    state.signalingReconnectAttempts=0;
    state.signalingReconnectBlocked=false;
    state.signalingDebug.clear();

    if(!ensureSignaling(state)) return;

    state.roomStateQueued=true;
    state.roomStateDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(1);
    state.queuedRoomState=target;
    state.queuedRoomCode=std::move(code);
    state.roomRequestPending=target!=1;
    state.error.clear();
}

void leaveRoom(UiState& state) {
    clearRoomMembership(state);
    state.roomCodeInput.fill('\0');
    queueRoomState(state,1);
}

void flushRoomState(UiState& state) {
    if(!state.roomStateQueued || std::chrono::steady_clock::now()<state.roomStateDeadline) return;
    if(!state.signaling) {
        resetRoomQueue(state);
        resetRoomRequest(state);
        return;
    }

    const int target=state.queuedRoomState;
    const std::string code=state.queuedRoomCode;
    resetRoomQueue(state);

    try {
        if(target==1) {
            state.signaling->setRoomNone();
        } else if(target==2) {
            state.signaling->setRoomCreate(state.signalingMemberId,state.playerName.data());
        } else if(target==3) {
            state.signaling->setRoomJoin(code,state.signalingMemberId,state.playerName.data());
                }

        state.inFlightRoomState=target;
        state.inFlightRoomCode=code;
        state.roomRequestPending=true;
        state.roomRequestSentAt=std::chrono::steady_clock::now();
    } catch(const std::exception& error) {
        scheduleSignalingReconnect(state,target,code,error.what());
    }
}

void recoverTimedOutRoomRequest(UiState& state) {
    if(!state.roomRequestPending || state.roomStateQueued || state.inFlightRoomState==0) return;
    if(state.roomRequestSentAt==std::chrono::steady_clock::time_point{}) return;
    if(std::chrono::steady_clock::now()-state.roomRequestSentAt<std::chrono::seconds(5)) return;

    if(state.roomRequestRetries>=2) {
        state.error="Signaling request timed out. Try again.";
        resetRoomRequest(state);
        state.roomRequestRetries=0;
        return;
    }

    if(!state.signaling) {
        const int target=state.inFlightRoomState;
        const std::string code=state.inFlightRoomCode;
        scheduleSignalingReconnect(state,target,code,"Signaling connection is unavailable.");
        return;
    }

    try {
        if(state.inFlightRoomState==1) {
            state.signaling->setRoomNone();
        } else if(state.inFlightRoomState==2) {
            state.signaling->setRoomCreate(state.signalingMemberId,state.playerName.data());
        } else if(state.inFlightRoomState==3) {
            state.signaling->setRoomJoin(state.inFlightRoomCode,state.signalingMemberId,state.playerName.data());
        }

        ++state.roomRequestRetries;
        state.roomRequestSentAt=std::chrono::steady_clock::now();
        state.error="Signaling request timed out. Retrying on existing connection...";
    } catch(const std::exception& error) {
        const int target=state.inFlightRoomState;
        const std::string code=state.inFlightRoomCode;
        scheduleSignalingReconnect(state,target,code,error.what());
    }
}

std::vector<std::string> parseIceServers(std::string_view text) {
    std::vector<std::string> servers;

    while(!text.empty()) {
        const auto end=text.find('\n');
        const auto line=text.substr(0,end);
        if(!line.empty()) {
            std::string server(line);
            if(!server.starts_with("stun:") && !server.starts_with("turn:") && !server.starts_with("turns:")) {
                throw std::runtime_error("Signaling server sent an invalid ICE server.");
            }
            servers.push_back(std::move(server));
        }

        if(end==std::string_view::npos) break;
        text.remove_prefix(end+1);
    }

    if(servers.empty()) throw std::runtime_error("Signaling server sent no ICE servers.");
    return servers;
}

bool hasTurnServer(const std::vector<std::string>& servers) {
    return std::any_of(servers.begin(),servers.end(),[](const std::string& server) {
        return server.starts_with("turn:") || server.starts_with("turns:");
    });
}


std::string decodeHexText(std::string_view value) {
    if(value.size()%2!=0) return {};
    std::string decoded;
    decoded.reserve(value.size()/2);

    const auto hexValue=[](char ch)->int {
        if(ch>='0' && ch<='9') return ch-'0';
        if(ch>='A' && ch<='F') return ch-'A'+10;
        if(ch>='a' && ch<='f') return ch-'a'+10;
        return -1;
    };

    for(std::size_t i=0;i<value.size();i+=2) {
        const int high=hexValue(value[i]);
        const int low=hexValue(value[i+1]);
        if(high<0 || low<0) return {};
        decoded.push_back(static_cast<char>((high<<4)|low));
    }
    return decoded;
}

std::vector<std::string> splitLines(std::string_view value) {
    std::vector<std::string> lines;
    while(true) {
        const auto end=value.find('\n');
        lines.emplace_back(value.substr(0,end));
        if(end==std::string_view::npos) break;
        value.remove_prefix(end+1);
    }
    return lines;
}

void applyRetroRewindStatus(UiState& state,const std::string& payload,bool verified) {
    const auto lines=splitLines(payload);
    if(lines.size()<5) throw std::runtime_error("Retro Rewind status response is incomplete.");

    state.rrVerifiedProfileId=lines[0];
    state.rrRoomId=lines[1];
    state.rrRoomInstanceId=lines[2];
    state.rrRoomCreated=lines[3];
    state.rrPlayers.clear();

    std::size_t expectedPlayers=0;
    try {
        expectedPlayers=static_cast<std::size_t>(std::stoul(lines[4]));
    } catch(...) {
        throw std::runtime_error("Retro Rewind player count is invalid.");
    }

    for(std::size_t i=5;i<lines.size();++i) {
        const auto first=lines[i].find('\t');
        if(first==std::string::npos) continue;
        const auto second=lines[i].find('\t',first+1);
        if(second==std::string::npos) continue;

        RetroRewindPlayerInfo player;
        player.participantId=lines[i].substr(0,first);
        player.voiceChat=lines[i].substr(first+1,second-first-1)=="1";
        player.name=decodeHexText(std::string_view(lines[i]).substr(second+1));
        if(player.name.empty()) player.name="Player";
        state.rrPlayers.push_back(std::move(player));
    }

    if(state.rrPlayers.size()!=expectedPlayers) {
        throw std::runtime_error("Retro Rewind roster response player count does not match.");
    }

    state.rrAuthPending=false;
    state.rrAuthenticated=verified;
    state.rrAuthStatus=verified
        ? "RR identity verified"
        : "Public roster lookup only - identity NOT verified";
    state.rrLastSync=std::chrono::steady_clock::now();
}

void startIcePeer(UiState& state,bool offerer) {
    resetIceTransport(state);

    auto iceServers=state.iceServers;
    if(iceServers.empty()) iceServers.push_back("stun:stun.l.google.com:19302");

    const bool relayOnly=state.connectionMode==1 && state.forceRelay;
    if(relayOnly && !hasTurnServer(iceServers)) {
        throw std::runtime_error("Signaling server did not provide a TURN server.");
    }

    state.iceRole=offerer ? 0 : 1;
    state.iceSetup=std::make_unique<mkwvc::IcePeerTransport>(std::move(iceServers),relayOnly);
    if(offerer) state.iceSetup->beginOffer();
}

void beginIce(UiState& state) {
    const bool offerer=state.iceRole==0;
    resetIce(state);
    state.error.clear();

    try {
        startIcePeer(state,offerer);
    } catch(const std::exception& error) {
        state.error=error.what();
        state.iceSetup.reset();
    }
}

void applyIceBundle(UiState& state,const mkwvc::IceSignalBundle& bundle,const char* expected) {
    if(!state.iceSetup) throw std::runtime_error("ICE connection is not initialized");
    if(bundle.description.type!=expected) throw std::runtime_error(std::string("Expected an ICE ")+expected+".");

    state.iceSetup->setRemoteDescription(bundle.description.sdp,bundle.description.type);
    for(const auto& candidate:bundle.candidates) {
        state.iceSetup->addRemoteCandidate(candidate.candidate,candidate.mid);
    }
    state.remoteIceApplied=true;
}

void applyRemoteIce(UiState& state) {
    if(!state.iceSetup) {
        state.error="Start an Internet connection first.";
        return;
    }

    try {
        const auto bundle=mkwvc::decodeIceSignal(state.remoteIceSignal.data());
        applyIceBundle(state,bundle,state.iceRole==0 ? "answer" : "offer");
        state.error.clear();
    } catch(const std::exception& error) {
        state.error=error.what();
    }
}

void beginRoom(UiState& state,bool create) {
    if(!state.activeRoomCode.empty()) return;

    const std::string code=state.roomCodeInput.data();
    if(!create && code.empty()) {
        state.error="Room code is empty";
        return;
    }

    clearRoomMembership(state);
    queueRoomState(state,create ? 2 : 3,code);
    saveState(state);
}

void applyCurrentPeerVolume(UiState& state) {
    if(!state.client) return;

    bool applied=false;
    for(const auto& peer:state.roomPeers) {
        if(!state.client->hasPeer(peer.memberId)) continue;
        state.client->setRemoteVolume(peer.memberId,peer.volume);
        applied=true;
    }

    if(!applied && state.client->peerCount()==1) {
        const float volume=state.roomPeers.size()==1 ? state.roomPeers.front().volume : 1.0f;
        state.client->setRemoteVolume(volume);
    }
}

RoomPeerLink* findRoomPeerLink(UiState& state,const std::string& memberId) {
    const auto found=std::find_if(state.roomPeerLinks.begin(),state.roomPeerLinks.end(),[&](const RoomPeerLink& link) {
        return link.memberId==memberId;
    });
    return found==state.roomPeerLinks.end() ? nullptr : &*found;
}

void removeRoomPeerLink(UiState& state,const std::string& memberId) {
    if(memberId.empty()) return;

    if(state.client) state.client->removePeer(memberId);
    std::erase_if(state.roomPeerLinks,[&](const RoomPeerLink& link) {
        return link.memberId==memberId;
    });

    state.roomPeerReady=!state.roomPeerLinks.empty();
    if(state.client && state.client->peerCount()==0) state.localAddress.clear();
}

RoomPeerLink& startRoomPeerLink(UiState& state,const std::string& memberId,bool offerer) {
    removeRoomPeerLink(state,memberId);

    auto iceServers=state.iceServers;
    if(iceServers.empty()) iceServers.push_back("stun:stun.l.google.com:19302");

    RoomPeerLink link;
    link.memberId=memberId;
    link.offerer=offerer;
    link.setup=std::make_unique<mkwvc::IcePeerTransport>(std::move(iceServers),false);
    if(offerer) link.setup->beginOffer();

    state.roomPeerLinks.push_back(std::move(link));
    state.roomPeerReady=true;
    return state.roomPeerLinks.back();
}

void applyRoomIceBundle(RoomPeerLink& link,const mkwvc::IceSignalBundle& bundle,const char* expected) {
    if(!link.setup) throw std::runtime_error("Room ICE connection is not initialized");
    if(bundle.description.type!=expected) throw std::runtime_error(std::string("Expected an ICE ")+expected+".");

    link.setup->setRemoteDescription(bundle.description.sdp,bundle.description.type);
    for(const auto& candidate:bundle.candidates) {
        link.setup->addRemoteCandidate(candidate.candidate,candidate.mid);
    }
    link.remoteApplied=true;
}

void upsertPeerInfo(UiState& state,std::string payload) {
    const auto first=payload.find('\n');
    if(first==std::string::npos) return;
    const auto second=payload.find('\n',first+1);
    if(second==std::string::npos) return;

    std::string memberId=payload.substr(0,first);
    std::string country=payload.substr(first+1,second-first-1);
    std::string name=payload.substr(second+1);
    if(memberId.empty()) return;
    if(country.empty()) country="??";
    if(name.empty()) name="Player";

    const auto found=std::find_if(state.roomPeers.begin(),state.roomPeers.end(),[&](const PeerUiInfo& peer) {
        return peer.memberId==memberId;
    });

    if(found!=state.roomPeers.end()) {
        found->name=std::move(name);
        found->country=std::move(country);
    } else {
        state.roomPeers.push_back({std::move(memberId),std::move(name),std::move(country),1.0f});
    }

    applyCurrentPeerVolume(state);
}

void removePeerInfo(UiState& state,const std::string& memberId) {
    if(memberId.empty()) {
        state.roomPeers.clear();
        applyCurrentPeerVolume(state);
        return;
    }

    std::erase_if(state.roomPeers,[&](const PeerUiInfo& peer) {
        return peer.memberId==memberId;
    });
    applyCurrentPeerVolume(state);
}

void pollRoomSignaling(UiState& state) {
    if(!state.signaling) return;

    state.signaling->service();

    for(auto& event:state.signaling->takeEvents()) {
        try {
            switch(event.type) {
                case mkwvc::SignalingEventType::RoomCreated: {
                    const int reconnectAttempt=state.signalingReconnectAttempts;
                    state.activeRoomCode=std::move(event.payload);
                    resetRoomRequest(state);
                    state.roomRequestRetries=0;
                    resetSignalingReconnect(state);
                    resetSignalingOpenWait(state);
                    state.signalingReconnectAttempts=0;
                    state.signalingReconnectBlocked=false;
                    if(reconnectAttempt>0) {
                        state.signalingDebug="Reconnect succeeded on attempt "+std::to_string(reconnectAttempt)+"/3.";
                    }
                    state.error.clear();
                    break;
                }
                case mkwvc::SignalingEventType::RoomJoined: {
                    const int reconnectAttempt=state.signalingReconnectAttempts;
                    state.activeRoomCode=std::move(event.payload);
                    resetRoomRequest(state);
                    state.roomRequestRetries=0;
                    resetSignalingReconnect(state);
                    resetSignalingOpenWait(state);
                    state.signalingReconnectAttempts=0;
                    state.signalingReconnectBlocked=false;
                    if(reconnectAttempt>0) {
                        state.signalingDebug="Reconnect succeeded on attempt "+std::to_string(reconnectAttempt)+"/3.";
                    }
                    state.error.clear();
                    break;
                }
                case mkwvc::SignalingEventType::RoomLeft: {
                    const int reconnectAttempt=state.signalingReconnectAttempts;
                    resetRoomRequest(state);
                    state.roomRequestRetries=0;
                    resetSignalingReconnect(state);
                    resetSignalingOpenWait(state);
                    state.signalingReconnectAttempts=0;
                    state.signalingReconnectBlocked=false;
                    if(reconnectAttempt>0) {
                        state.signalingDebug="Reconnect succeeded on attempt "+std::to_string(reconnectAttempt)+"/3.";
                    }
                    state.error.clear();
                    break;
                }
                case mkwvc::SignalingEventType::IceServers:
                    state.iceServers=parseIceServers(event.payload);
                    state.roomIceServersReady=true;
                    break;
                case mkwvc::SignalingEventType::PeerReady: {
                    if(state.activeRoomCode.empty() || state.roomStateQueued) break;

                    if(event.memberId.empty()) {
                        resetIceTransport(state);
                        state.roomPeerReady=true;
                        state.error.clear();
                        if(state.roomIceServersReady) startIcePeer(state,true);
                        break;
                    }

                    state.memberAddressedSignaling=true;
                    if(!state.roomIceServersReady) throw std::runtime_error("Waiting for ICE server configuration.");

                    const bool offerer=state.signalingMemberId<event.memberId;
                    startRoomPeerLink(state,event.memberId,offerer);
                    state.error.clear();
                    break;
                }
                case mkwvc::SignalingEventType::PeerInfo:
                    upsertPeerInfo(state,std::move(event.payload));
                    break;
                case mkwvc::SignalingEventType::Signal: {
                    if(state.activeRoomCode.empty() || state.roomStateQueued) break;

                    const auto bundle=mkwvc::decodeIceSignal(event.payload);

                    if(event.memberId.empty()) {
                        if(!state.iceSetup) {
                            if(!state.roomIceServersReady) throw std::runtime_error("Waiting for ICE server configuration.");
                            if(bundle.description.type!="offer") throw std::runtime_error("Received ICE answer without a local offer.");
                            startIcePeer(state,false);
                        }
                        applyIceBundle(state,bundle,state.iceRole==0 ? "answer" : "offer");
                        state.error.clear();
                        break;
                    }

                    state.memberAddressedSignaling=true;
                    auto* link=findRoomPeerLink(state,event.memberId);
                    if(!link) {
                        if(!state.roomIceServersReady) throw std::runtime_error("Waiting for ICE server configuration.");
                        if(bundle.description.type!="offer") throw std::runtime_error("Received ICE answer without a local offer.");
                        link=&startRoomPeerLink(state,event.memberId,false);
                    }

                    applyRoomIceBundle(*link,bundle,link->offerer ? "answer" : "offer");
                    state.error.clear();
                    break;
                }
                case mkwvc::SignalingEventType::PeerLeft:
                    if(state.activeRoomCode.empty() || state.roomStateQueued) break;
                    removePeerInfo(state,event.payload);

                    if(event.payload.empty()) {
                        resetIceTransport(state);
                        state.roomPeerReady=false;
                    } else {
                        removeRoomPeerLink(state,event.payload);
                    }

                    state.error="Peer left the room.";
                    break;
                case mkwvc::SignalingEventType::RetroRewindStatus:
                    applyRetroRewindStatus(state,event.payload,true);
                    break;
                case mkwvc::SignalingEventType::RetroRewindDebugStatus:
                    applyRetroRewindStatus(state,event.payload,false);
                    break;
                case mkwvc::SignalingEventType::RetroRewindDebugFailed:
                    state.rrAuthPending=false;
                    state.rrAuthenticated=false;
                    state.rrAuthStatus=event.payload.empty() ? "Roster lookup failed" : event.payload;
                    state.rrRoomId.clear();
                    state.rrRoomInstanceId.clear();
                    state.rrRoomCreated.clear();
                    state.rrPlayers.clear();
                    break;
                case mkwvc::SignalingEventType::RetroRewindAuthFailed:
                    state.rrAuthPending=false;
                    state.rrAuthenticated=false;
                    state.rrAuthStatus=event.payload.empty() ? "Authentication failed" : event.payload;
                    state.rrVerifiedProfileId.clear();
                    state.rrRoomId.clear();
                    state.rrRoomInstanceId.clear();
                    state.rrRoomCreated.clear();
                    state.rrPlayers.clear();
                    break;
                case mkwvc::SignalingEventType::RetroRewindAuthRequired:
                    state.rrAuthPending=false;
                    state.rrAuthenticated=false;
                    state.rrAuthStatus="Authentication expired; authenticate again.";
                    state.rrRoomId.clear();
                    state.rrRoomInstanceId.clear();
                    state.rrRoomCreated.clear();
                    state.rrPlayers.clear();
                    break;
                case mkwvc::SignalingEventType::TransportError: {
                    if(state.rrAuthenticated || state.rrAuthPending) {
                        state.rrAuthenticated=false;
                        state.rrAuthPending=false;
                        state.rrAuthStatus="Signaling connection lost; authenticate again.";
                    }
                    auto [target,code]=desiredReconnectState(state);
                    const bool hadRoom=!state.activeRoomCode.empty();
                    const std::string error=event.payload.empty() ? "Signaling transport error." : "Signaling transport error: "+event.payload;

                    if(hadRoom) {
                        resetIceTransport(state);
                        state.roomPeerReady=false;
                        state.roomIceServersReady=false;
                        state.iceServers.clear();
                    }
                    scheduleSignalingReconnect(state,target,std::move(code),error);
                    break;
                }
                case mkwvc::SignalingEventType::Error: {
                    const std::string error=std::move(event.payload);
                    if(state.rrAuthPending) {
                        state.rrAuthPending=false;
                        state.rrAuthStatus=error;
                    }
                    const bool recoverable=
                        error=="Already in a room" ||
                        error=="Unknown command";
                    const bool retry=
                        recoverable &&
                        state.roomRequestPending &&
                        state.inFlightRoomState!=0 &&
                        state.roomRequestRetries<2;
                    const int target=state.inFlightRoomState;
                    const std::string code=state.inFlightRoomCode;

                    resetRoomRequest(state);

                    if(retry) {
                        scheduleSignalingReconnect(state,target,code,"Signaling state out of sync.");
                    } else {
                        state.roomRequestRetries=0;
                        if(target==3 && (error=="Room not found" || error=="Room is full")) {
                            clearRoomMembership(state);
                        }
                        state.error=error;
                    }
                    break;
                }
                case mkwvc::SignalingEventType::Closed: {
                    if(state.rrAuthenticated || state.rrAuthPending) {
                        state.rrAuthenticated=false;
                        state.rrAuthPending=false;
                        state.rrAuthStatus="Signaling connection closed; authenticate again.";
                    }
                    auto [target,code]=desiredReconnectState(state);
                    const bool hadRoom=!state.activeRoomCode.empty();

                    if(hadRoom) {
                        resetIceTransport(state);
                        state.roomPeerReady=false;
                        state.roomIceServersReady=false;
                        state.iceServers.clear();
                    }
                    scheduleSignalingReconnect(state,target,std::move(code),"Signaling server disconnected.");
                    break;
                }
                case mkwvc::SignalingEventType::Open: {
                    resetSignalingOpenWait(state);
                    if(state.signalingReconnectAttempts>0 &&
                       !state.roomStateQueued &&
                       !state.roomRequestPending) {
                        const int reconnectAttempt=state.signalingReconnectAttempts;
                        resetSignalingReconnect(state);
                        state.signalingReconnectAttempts=0;
                        state.signalingReconnectBlocked=false;
                        state.signalingDebug="Reconnect succeeded on attempt "+std::to_string(reconnectAttempt)+"/3.";
                        state.error.clear();
                    } else if(state.signalingReconnectAttempts>0) {
                        state.signalingDebug=
                            "Reconnect attempt "+std::to_string(state.signalingReconnectAttempts)+
                            "/3 WebSocket connected; restoring room...";
                    }
                    break;
                }
            }
        } catch(const std::exception& error) {
            state.error=error.what();
        }
    }
}

void pollIce(UiState& state) {
    const auto configureClient=[&](mkwvc::VoiceClient& client) {
        client.setCodecSettings(state.codecSettings);
        client.setNetworkSimulation(state.networkSimulation);
        client.setMicrophoneGain(state.microphoneGain);
        client.setAudioProcessingSettings(state.audioProcessing);
        client.setPlaybackVolume(state.playbackVolume);
        client.setTransmitEnabled(!state.microphoneMuted && !state.deafened && !state.pushToTalk);
        client.setDeafened(state.deafened);
        client.setMicrophoneTestEnabled(state.microphoneTest);
    };

    if(state.connectionMode==1 && state.memberAddressedSignaling) {
        for(auto& link:state.roomPeerLinks) {
            if(!link.setup) continue;

            if(auto description=link.setup->takeLocalDescription()) {
                link.localDescription=std::move(*description);
            }

            auto candidates=link.setup->takeLocalCandidates();
            link.localCandidates.insert(
                link.localCandidates.end(),
                std::make_move_iterator(candidates.begin()),
                std::make_move_iterator(candidates.end())
            );

            if(link.localDescription && link.setup->gatheringComplete() && !link.signalSent) {
                try {
                    link.localSignal=mkwvc::encodeIceSignal({
                        *link.localDescription,
                        link.localCandidates
                    });
                    if(state.signaling) {
                        state.signaling->sendSignal(link.memberId,link.localSignal);
                        link.signalSent=true;
                    }
                } catch(const std::exception& error) {
                    state.error=error.what();
                }
            }

            if(!link.setup->connected() ||
               !link.localDescription ||
               !link.setup->gatheringComplete()) {
                continue;
            }

            if(state.client && state.client->hasPeer(link.memberId)) continue;

            try {
                auto* raw=link.setup.get();
                auto transport=std::unique_ptr<mkwvc::VoiceTransport>(link.setup.release());

                if(!state.client) {
                    stopLocalMicrophoneTest(state);
                    auto client=std::make_unique<mkwvc::VoiceClient>(
                        link.memberId,
                        std::move(transport),
                        selectedInputDevice(state),
                        selectedOutputDevice(state)
                    );
                    configureClient(*client);
                    client->start();
                    state.client=std::move(client);
                    resetRates(state);
                } else {
                    state.client->addPeer(link.memberId,std::move(transport));
                }

                link.transport=raw;
                if(state.localAddress.empty()) state.localAddress=raw->localAddress();
                applyCurrentPeerVolume(state);
                state.error.clear();
            } catch(const std::exception& error) {
                state.error=error.what();
                link.transport=nullptr;
            }
        }

        state.roomPeerReady=!state.roomPeerLinks.empty();
        return;
    }

    if(state.iceSetup) {
        if(auto description=state.iceSetup->takeLocalDescription()) {
            state.localIceDescription=std::move(*description);
        }

        auto candidates=state.iceSetup->takeLocalCandidates();
        state.localIceCandidates.insert(
            state.localIceCandidates.end(),
            std::make_move_iterator(candidates.begin()),
            std::make_move_iterator(candidates.end())
        );

        if(state.localIceDescription && state.iceSetup->gatheringComplete()) {
            try {
                state.localIceSignal=mkwvc::encodeIceSignal({
                    *state.localIceDescription,
                    state.localIceCandidates
                });

                if(state.connectionMode==1 && state.signaling && state.roomPeerReady && !state.roomSignalSent) {
                    state.signaling->sendSignal(state.localIceSignal);
                    state.roomSignalSent=true;
                }
            } catch(const std::exception& error) {
                state.error=error.what();
            }
        }

        if(state.iceSetup->connected() && state.localIceDescription &&
           state.iceSetup->gatheringComplete() && !state.client) {
            try {
                stopLocalMicrophoneTest(state);
                auto* raw=state.iceSetup.get();
                auto transport=std::unique_ptr<mkwvc::VoiceTransport>(state.iceSetup.release());
                auto client=std::make_unique<mkwvc::VoiceClient>(
                    std::move(transport),
                    selectedInputDevice(state),
                    selectedOutputDevice(state)
                );
                configureClient(*client);
                state.iceTransport=raw;
                state.localAddress=raw->localAddress();
                client->start();
                state.client=std::move(client);
                applyCurrentPeerVolume(state);
                resetRates(state);
            } catch(const std::exception& error) {
                state.error=error.what();
                state.iceTransport=nullptr;
            }
        }
    }

    if(state.iceTransport) state.localAddress=state.iceTransport->localAddress();
}
#endif

void updateLiveVoiceControls(UiState& state) {
    const bool pttHeld=pushToTalkHeld(state);
    const bool microphoneTestAudible=state.microphoneTest && pttHeld;

    if(state.localMicrophoneTest) state.localMicrophoneTest->setMonitorEnabled(microphoneTestAudible);
    if(!state.client) return;

    const bool transmitEnabled=!state.microphoneTest && !state.microphoneMuted && !state.deafened && pttHeld;

    state.client->setTransmitEnabled(transmitEnabled);
    state.client->setDeafened(state.deafened || state.microphoneTest);
    state.client->setMicrophoneTestEnabled(microphoneTestAudible);

#ifdef MKWVC_HAS_ICE
    if(state.connectionMode==1) applyCurrentPeerVolume(state);
    else state.client->setRemoteVolume(1.0f);
#else
    state.client->setRemoteVolume(1.0f);
#endif
}

void startLanClient(UiState& state) {
#ifdef MKWVC_HAS_ICE
    resetIce(state);
#endif
    stopClient(state);
    state.error.clear();

    if(state.peer[0]=='\0') {
        state.error="Enter the peer IPv4 address.";
        return;
    }

    if(state.port<1 || state.port>65535) {
        state.error="Port must be between 1 and 65535.";
        return;
    }

    try {
        stopLocalMicrophoneTest(state);
        auto client=std::make_unique<mkwvc::VoiceClient>(state.peer.data(),static_cast<std::uint16_t>(state.port),selectedInputDevice(state),selectedOutputDevice(state));
        client->setCodecSettings(state.codecSettings);
        client->setNetworkSimulation(state.networkSimulation);
        client->setMicrophoneGain(state.microphoneGain);
        client->setAudioProcessingSettings(state.audioProcessing);
        client->setPlaybackVolume(state.playbackVolume);
        client->setRemoteVolume(1.0f);
        client->setTransmitEnabled(!state.microphoneMuted && !state.deafened && !state.pushToTalk);
        client->setDeafened(state.deafened);
        client->setMicrophoneTestEnabled(state.microphoneTest);
        state.localAddress=client->localAddress();
        client->start();
        state.client=std::move(client);
        resetRates(state);
        saveState(state);
    } catch(const std::exception& error) {
        state.error=error.what();
        state.client.reset();
    }
}

void updateRates(UiState& state,const mkwvc::VoiceStats& stats) {
    const auto now=std::chrono::steady_clock::now();

    if(stats.rxPackets!=state.observedRxPackets) {
        state.observedRxPackets=stats.rxPackets;
        state.lastPeerPacket=now;
        state.peerSeen=true;
    }

    if(now-state.lastRateUpdate<std::chrono::seconds(1)) return;

    state.txRate=stats.txPackets-state.lastTx;
    state.rxRate=stats.rxPackets-state.lastRx;
    state.txByteRate=stats.txBytes-state.lastTxBytes;
    state.rxByteRate=stats.rxBytes-state.lastRxBytes;
    state.lastTx=stats.txPackets;
    state.lastRx=stats.rxPackets;
    state.lastTxBytes=stats.txBytes;
    state.lastRxBytes=stats.rxBytes;
    state.lastRateUpdate=now;
}

void renderInputDevice(UiState& state) {
    ImGui::TextUnformatted("Audio input");

    const std::string preview=state.inputDeviceIndex==0 ? "System default" : selectedInputDevice(state);
    const auto& style=ImGui::GetStyle();
    const float refreshWidth=ImGui::CalcTextSize("Refresh").x+style.FramePadding.x*2.0f;
    const float available=ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(std::max(120.0f,available-refreshWidth-style.ItemSpacing.x));

    if(ImGui::BeginCombo("##InputDevice",preview.c_str())) {
        if(ImGui::Selectable("System default",state.inputDeviceIndex==0)) changeInputDevice(state,0);
        for(std::size_t i=0;i<state.inputDevices.size();++i) {
            const bool selected=state.inputDeviceIndex==static_cast<int>(i+1);
            if(ImGui::Selectable(state.inputDevices[i].c_str(),selected)) changeInputDevice(state,static_cast<int>(i+1));
            if(selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    if(ImGui::Button("Refresh")) refreshInputDevices(state);
}

void renderVoiceProcessing(UiState& state) {
    ImGui::TextUnformatted("Voice processing");

    if(ImGui::Checkbox("Automatic normalization",&state.audioProcessing.normalization)) {
        applyAudioProcessingSettings(state);
        saveState(state);
    }

    if(ImGui::Checkbox("Noise suppression",&state.audioProcessing.noiseSuppression)) {
        applyAudioProcessingSettings(state);
        saveState(state);
    }
    if(state.audioProcessing.noiseSuppression) {
        ImGui::SetNextItemWidth(-1.0f);
        if(ImGui::SliderInt("##NoiseSuppressionStrength",&state.audioProcessing.noiseSuppressionStrength,0,100,"Noise strength %d%%")) {
            applyAudioProcessingSettings(state);
        }
        if(ImGui::IsItemDeactivatedAfterEdit()) saveState(state);
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Microphone gain");
    int gainPercent=static_cast<int>(state.microphoneGain*100.0f+0.5f);
    if(ImGui::SliderInt("##MicrophoneGain",&gainPercent,25,500,"%d%%")) {
        state.microphoneGain=static_cast<float>(gainPercent)/100.0f;
        applyMicrophoneGain(state);
    }
    if(ImGui::IsItemDeactivatedAfterEdit()) saveState(state);

    ImGui::TextDisabled("Normalization runs before the manual microphone gain.");
}

void renderMicrophoneTest(UiState& state,const mkwvc::VoiceStats& stats) {
    ImGui::TextUnformatted("Microphone test");
    bool enabled=state.microphoneTest;
    if(ImGui::Checkbox("Hear my processed microphone",&enabled)) {
        if(enabled) startMicrophoneTest(state);
        else stopMicrophoneTest(state);
    }

    if(!state.microphoneTest) {
        ImGui::TextDisabled("Start the test to hear normalization, noise suppression and gain.");
        return;
    }

    if(!state.client && !state.localMicrophoneTest) startLocalMicrophoneTest(state);
    const auto peak=state.client ? stats.micPeak : (state.localMicrophoneTest ? state.localMicrophoneTest->micPeak() : 0);
    ImGui::ProgressBar(normalizePeak(peak),ImVec2(-1.0f,20.0f),"");
    if(state.pushToTalk) ImGui::TextDisabled("Hold V to hear your processed microphone.");
    ImGui::TextDisabled("Test mode deafens remote audio and never transmits your microphone.");
}

void renderOutputDevice(UiState& state) {
    ImGui::TextUnformatted("Audio output");

    const std::string preview=state.outputDeviceIndex==0 ? "System default" : selectedOutputDevice(state);
    const auto& style=ImGui::GetStyle();
    const float refreshWidth=ImGui::CalcTextSize("Refresh##Output").x+style.FramePadding.x*2.0f;
    const float available=ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(std::max(120.0f,available-refreshWidth-style.ItemSpacing.x));

    if(ImGui::BeginCombo("##OutputDevice",preview.c_str())) {
        if(ImGui::Selectable("System default",state.outputDeviceIndex==0)) changeOutputDevice(state,0);
        for(std::size_t i=0;i<state.outputDevices.size();++i) {
            const bool selected=state.outputDeviceIndex==static_cast<int>(i+1);
            if(ImGui::Selectable(state.outputDevices[i].c_str(),selected)) changeOutputDevice(state,static_cast<int>(i+1));
            if(selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    if(ImGui::Button("Refresh##Output")) refreshOutputDevices(state);

    ImGui::Spacing();
    ImGui::TextUnformatted("Output volume");
    int volumePercent=static_cast<int>(state.playbackVolume*100.0f+0.5f);
    if(ImGui::SliderInt("##PlaybackVolume",&volumePercent,0,300,"%d%%")) {
        state.playbackVolume=static_cast<float>(volumePercent)/100.0f;
        applyPlaybackVolume(state);
    }
    if(ImGui::IsItemDeactivatedAfterEdit()) saveState(state);
}

void renderInterfaceTab(UiState& state,const mkwvc::VoiceStats& stats) {
    ImGui::TextUnformatted("Profile");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextUnformatted("Display name");
    ImGui::SetNextItemWidth(-1.0f);
    if(ImGui::InputText("##DisplayName",state.playerName.data(),state.playerName.size())) {
        if(state.playerName[0]=='\0') std::snprintf(state.playerName.data(),state.playerName.size(),"%s","Player");
    }
    if(ImGui::IsItemDeactivatedAfterEdit()) saveState(state);

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::TextUnformatted("Audio");
    ImGui::Separator();
    ImGui::Spacing();

    renderInputDevice(state);
    ImGui::Spacing();
    renderVoiceProcessing(state);
    ImGui::Spacing();
    renderOutputDevice(state);
    ImGui::Spacing();
    renderMicrophoneTest(state,stats);

    ImGui::Spacing();
    ImGui::BeginDisabled(state.microphoneTest || state.pushToTalk);
    if(ImGui::Button(state.microphoneMuted ? "Unmute" : "Mute")) {
        state.microphoneMuted=!state.microphoneMuted;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(state.microphoneTest);
    if(ImGui::Button(state.deafened ? "Undeafen" : "Deafen")) {
        state.deafened=!state.deafened;
    }
    ImGui::SameLine();
    if(ImGui::Checkbox("Push-to-talk",&state.pushToTalk) && state.pushToTalk) {
        state.microphoneMuted=false;
    }
    ImGui::EndDisabled();

    if(state.pushToTalk && !state.microphoneTest) {
        ImGui::TextDisabled("Hold V to talk");
    }

    if(state.microphoneTest) {
        ImGui::TextDisabled("Microphone test active.");
    } else if(state.deafened) {
        ImGui::TextDisabled("Deafened: incoming audio and microphone are muted.");
    } else if(state.microphoneMuted) {
        ImGui::TextDisabled("Muted: microphone is muted.");
    } else if(state.pushToTalk) {
        ImGui::TextDisabled(ImGui::IsKeyDown(ImGuiKey_V) ? "Push-to-talk active." : "Push-to-talk waiting.");
    }

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::TextUnformatted("Voice activity");
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextUnformatted("Sending");
    ImGui::ProgressBar(normalizePeak(stats.micPeak),ImVec2(-1.0f,20.0f),"");

#ifdef MKWVC_HAS_ICE
    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::TextUnformatted("Peers");
    ImGui::Separator();
    ImGui::Spacing();

    if(state.roomPeers.empty()) {
        ImGui::TextDisabled(state.activeRoomCode.empty() ? "Not in a room." : "No peer connected.");
    } else {
        if(ImGui::BeginTable("PeerList",3,ImGuiTableFlags_SizingStretchProp|ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Player",ImGuiTableColumnFlags_WidthStretch,1.2f);
            ImGui::TableSetupColumn("Volume",ImGuiTableColumnFlags_WidthStretch,2.0f);
            ImGui::TableSetupColumn("##Mute",ImGuiTableColumnFlags_WidthFixed,72.0f);

            for(std::size_t i=0;i<state.roomPeers.size();++i) {
                auto& peer=state.roomPeers[i];
                ImGui::PushID(peer.memberId.c_str());
                ImGui::TableNextRow();

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(peer.name.c_str());
                ImGui::SameLine();
                ImGui::TextDisabled("[%s]",peer.country.c_str());

                ImGui::TableNextColumn();
                int peerVolumePercent=static_cast<int>(peer.volume*100.0f+0.5f);
                ImGui::SetNextItemWidth(-1.0f);
                if(ImGui::SliderInt("##PeerVolume",&peerVolumePercent,0,300,"%d%%")) {
                    peer.volume=static_cast<float>(peerVolumePercent)/100.0f;
                    applyCurrentPeerVolume(state);
                }

                ImGui::TableNextColumn();
                ImGui::BeginDisabled(peer.volume<=0.0f);
                if(ImGui::Button("Mute",ImVec2(-1.0f,0.0f))) {
                    peer.volume=0.0f;
                    applyCurrentPeerVolume(state);
                }
                ImGui::EndDisabled();

                ImGui::PopID();
            }

            ImGui::EndTable();
        }

        ImGui::TextDisabled("Peer volume is session-only for now and is not saved.");
    }
#endif
}

void renderLanConnection(UiState& state,bool active) {
    ImGui::TextUnformatted("Local IPv4");
    ImGui::TextDisabled("%s",state.localAddress.empty() ? "Detected after Start" : state.localAddress.c_str());

    ImGui::Spacing();
    ImGui::BeginDisabled(active);
    ImGui::TextUnformatted("Peer IPv4");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##PeerIPv4",state.peer.data(),state.peer.size());

    ImGui::Spacing();
    ImGui::TextUnformatted("Port");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputInt("##Port",&state.port,0,0);
    ImGui::EndDisabled();

    const float buttonWidth=std::min(220.0f,ImGui::GetContentRegionAvail().x);
    if(active) {
        if(ImGui::Button("Stop",ImVec2(buttonWidth,42.0f))) stopClient(state);
    } else {
        if(ImGui::Button("Start LAN",ImVec2(buttonWidth,42.0f))) startLanClient(state);
    }

    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();

    if(!active) {
        ImGui::TextUnformatted("Stopped");
    } else {
        const bool peerOnline=state.peerSeen && std::chrono::steady_clock::now()-state.lastPeerPacket<std::chrono::seconds(3);
        ImGui::TextUnformatted(peerOnline ? "Peer online" : "Waiting for peer");
    }
}

#ifdef MKWVC_HAS_ICE
void renderRoomConnection(UiState& state,bool active) {
    const bool signalingActive=state.signaling!=nullptr;
    const bool roomActive=!state.activeRoomCode.empty();
    const bool waitingForServer=state.roomRequestPending && !state.roomStateQueued;

    ImGui::BeginDisabled(signalingActive);
    ImGui::TextUnformatted("Signaling server");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##SignalingServer",state.signalingServer.data(),state.signalingServer.size());
    if(ImGui::IsItemDeactivatedAfterEdit()) saveState(state);
    ImGui::EndDisabled();

    if(!state.signalingDebug.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("%s",state.signalingDebug.c_str());
    }

    ImGui::Spacing();

    if(!roomActive) {
        const float buttonWidth=std::min(220.0f,ImGui::GetContentRegionAvail().x);

        ImGui::BeginDisabled(waitingForServer);
        if(ImGui::Button("Create room",ImVec2(buttonWidth,42.0f))) beginRoom(state,true);

        ImGui::Spacing();
        ImGui::TextUnformatted("Room code");
        ImGui::SetNextItemWidth(std::max(140.0f,ImGui::GetContentRegionAvail().x-buttonWidth-10.0f));
        ImGui::InputText("##RoomCode",state.roomCodeInput.data(),state.roomCodeInput.size(),ImGuiInputTextFlags_CharsUppercase);
        ImGui::SameLine();
        ImGui::BeginDisabled(state.roomCodeInput[0]=='\0');
        if(ImGui::Button("Join room",ImVec2(buttonWidth,0.0f))) beginRoom(state,false);
        ImGui::EndDisabled();
        ImGui::EndDisabled();

        if(state.roomStateQueued) {
            ImGui::Spacing();
            const char* queued=
                state.queuedRoomState==1 ? "disconnected" :
                state.queuedRoomState==2 ? "create room" :
                state.queuedRoomState==3 ? "join room" :
                "idle";
            ImGui::TextDisabled("Queued final state: %s",queued);
            ImGui::TextDisabled("Final room state sends after 1 second without changes.");
            if(ImGui::Button("Cancel queued change")) {
                clearRoomMembership(state);
                queueRoomState(state,1);
            }
        } else if(waitingForServer) {
            ImGui::Spacing();
            ImGui::TextDisabled("Waiting for signaling server...");
        }

        if(signalingActive) {
            ImGui::Spacing();
            ImGui::TextDisabled("Signaling connection kept open for room changes.");
            ImGui::BeginDisabled(state.roomStateQueued || waitingForServer);
            if(ImGui::Button("Disconnect signaling")) {
                resetRoomQueue(state);
                resetRoomRequest(state);
                resetSignalingReconnect(state);
                resetSignalingOpenWait(state);
                state.signalingReconnectAttempts=0;
                state.signalingReconnectBlocked=false;
                state.signalingDebug.clear();
                state.signaling.reset();
            }
            ImGui::EndDisabled();
        }
        return;
    }

    ImGui::Text("Room code: %s",state.activeRoomCode.c_str());
    ImGui::SameLine();
    if(ImGui::Button("Copy code")) ImGui::SetClipboardText(state.activeRoomCode.c_str());

    const auto connectedPeers=state.client ? state.client->peerCount() : 0;
    if(connectedPeers>0) {
        ImGui::Spacing();
        ImGui::Text("ICE: %zu peer%s connected",connectedPeers,connectedPeers==1 ? "" : "s");
        ImGui::TextDisabled("Local endpoint: %s",state.localAddress.empty() ? "-" : state.localAddress.c_str());
    } else if(state.roomPeerLinks.empty()) {
        ImGui::TextDisabled("Waiting for someone to join...");
    } else {
        ImGui::TextDisabled("Connecting to %zu peer%s...",state.roomPeerLinks.size(),state.roomPeerLinks.size()==1 ? "" : "s");
    }

    ImGui::Spacing();
    if(ImGui::Button("Leave room")) leaveRoom(state);
}
void renderIceConnection(UiState& state,bool active) {
    const bool setup=state.iceSetup!=nullptr;
    const bool locked=active || setup;

    ImGui::BeginDisabled(locked);
    const char* roles[]={"Create offer","Answer offer"};
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::Combo("Role",&state.iceRole,roles,2);
    ImGui::EndDisabled();

    if(!setup && !active) {
        if(ImGui::Button("Start Internet connection",ImVec2(std::min(260.0f,ImGui::GetContentRegionAvail().x),42.0f))) {
            beginIce(state);
        }
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(state.iceRole==0 ? "Creates an offer" : "Waits for an offer");
        return;
    }

    if(state.iceSetup) {
        ImGui::TextDisabled("%s",
            !state.localIceDescription ? "Creating local description..." :
            !state.iceSetup->gatheringComplete() ? "Gathering ICE candidates..." :
            state.localIceSignal.empty() ? "Preparing share code..." :
            "Local share code ready"
        );
    }

    if(!state.localIceSignal.empty()) {
        ImGui::Spacing();
        ImGui::TextUnformatted(state.iceRole==0 ? "1. Send this offer to your friend" : "2. Send this answer back");
        ImGui::InputTextMultiline(
            "##LocalIceSignal",
            const_cast<char*>(state.localIceSignal.c_str()),
            state.localIceSignal.size()+1,
            ImVec2(-1.0f,100.0f),
            ImGuiInputTextFlags_ReadOnly
        );
        if(ImGui::Button("Copy local code")) ImGui::SetClipboardText(state.localIceSignal.c_str());
    }

    ImGui::Spacing();
    ImGui::TextUnformatted(state.iceRole==0 ? "2. Paste the answer from your friend" : "1. Paste the offer from your friend");
    ImGui::InputTextMultiline(
        "##RemoteIceSignal",
        state.remoteIceSignal.data(),
        state.remoteIceSignal.size(),
        ImVec2(-1.0f,100.0f)
    );

    if(ImGui::Button("Paste from clipboard")) {
        if(const char* clipboard=ImGui::GetClipboardText()) {
            std::snprintf(state.remoteIceSignal.data(),state.remoteIceSignal.size(),"%s",clipboard);
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(state.remoteIceSignal[0]=='\0' || state.remoteIceApplied);
    if(ImGui::Button("Apply remote code")) applyRemoteIce(state);
    ImGui::EndDisabled();

    if(state.remoteIceApplied && !active) {
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Connecting...");
    }

    if(active && state.iceTransport) {
        ImGui::Spacing();
        ImGui::Text("ICE: Connected (%s)",state.iceTransport->usingRelay() ? "Relayed" : "Direct P2P");
        const auto remote=state.iceTransport->remoteAddress();
        ImGui::TextDisabled("Local endpoint: %s",state.localAddress.empty() ? "-" : state.localAddress.c_str());
        ImGui::TextDisabled("Remote endpoint: %s",remote.empty() ? "-" : remote.c_str());
        if(const auto rtt=state.iceTransport->rttMilliseconds()) {
            ImGui::TextDisabled("RTT: %u ms",*rtt);
        }
    }

    ImGui::Spacing();
    if(ImGui::Button("Reset Internet connection")) resetIce(state);
}
#endif

void renderConnection(UiState& state,bool active) {
    ImGui::TextUnformatted("Connection");
    ImGui::Separator();
    ImGui::Spacing();

#ifdef MKWVC_HAS_ICE
    const bool internetBusy=state.iceSetup!=nullptr || state.signaling!=nullptr;
    ImGui::BeginDisabled(active || internetBusy);
    const char* modes[]={"LAN / direct UDP","Internet / Room code","Internet / Manual ICE"};
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::Combo("Mode",&state.connectionMode,modes,3);
    ImGui::EndDisabled();
    ImGui::Spacing();

    if(state.connectionMode==1) renderRoomConnection(state,active);
    else if(state.connectionMode==2) renderIceConnection(state,active);
    else renderLanConnection(state,active);
#else
    renderLanConnection(state,active);
#endif
}

void renderStats(UiState& state,const mkwvc::VoiceStats& stats) {
    ImGui::TextUnformatted("Voice activity");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("Microphone  %u",stats.micPeak);
    ImGui::ProgressBar(normalizePeak(stats.micPeak),ImVec2(-1.0f,20.0f),"");

    ImGui::Spacing();
    ImGui::Text("Playback  %u",stats.playbackPeak);
    ImGui::ProgressBar(normalizePeak(stats.playbackPeak),ImVec2(-1.0f,20.0f),"");

    ImGui::Spacing();
    if(ImGui::BeginTable("Traffic",2,ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn();
        ImGui::TextUnformatted("TX");
        ImGui::Text("%llu packets/s",static_cast<unsigned long long>(state.txRate));
        ImGui::Text("%.1f kbit/s payload",static_cast<double>(state.txByteRate)*8.0/1000.0);

        ImGui::TableNextColumn();
        ImGui::TextUnformatted("RX");
        ImGui::Text("%llu packets/s",static_cast<unsigned long long>(state.rxRate));
        ImGui::Text("%.1f kbit/s payload",static_cast<double>(state.rxByteRate)*8.0/1000.0);
        ImGui::EndTable();
    }

    if(stats.running && stats.protocolMismatchPackets>0 && stats.rxPackets==0) {
        ImGui::Spacing();
        ImGui::TextWrapped("Voice packets are reaching this client, but the peer uses an incompatible protocol version. Update and rebuild both clients.");
    } else if(stats.running && stats.rawRxPackets==0) {
        ImGui::Spacing();
#ifdef MKWVC_HAS_ICE
        if(state.connectionMode!=0) ImGui::TextWrapped("No voice packets received yet.");
        else
#endif
        ImGui::TextWrapped("No UDP packets received yet. Check both peer IPv4 addresses and make sure both clients are running.");
    }
}

void renderVoiceTab(UiState& state,const mkwvc::VoiceStats& stats,bool active) {
    renderConnection(state,active);
    ImGui::Spacing();

    if(!state.error.empty()) {
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextWrapped("%s",state.error.c_str());
        ImGui::Spacing();
    }

    renderStats(state,stats);
}

void renderCodecDebug(UiState& state) {
    ImGui::TextUnformatted("Opus codec");
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextDisabled("48 kHz mono, 20 ms frames, VBR");

    int bitrateKbps=state.codecSettings.bitrate/1000;
    if(ImGui::SliderInt("Bitrate",&bitrateKbps,12,64,"%d kbit/s")) state.codecSettings.bitrate=bitrateKbps*1000;
    ImGui::SliderInt("Complexity",&state.codecSettings.complexity,0,10);
    ImGui::SliderInt("Expected packet loss",&state.codecSettings.expectedPacketLossPercent,0,30,"%d%%");
    ImGui::Checkbox("In-band FEC",&state.codecSettings.inbandFec);
    ImGui::Checkbox("DTX",&state.codecSettings.dtx);

    if(state.client) state.client->setCodecSettings(state.codecSettings);
}

void renderNetworkSimulation(UiState& state) {
    ImGui::TextUnformatted("Outgoing network simulation");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Checkbox("Enable simulation",&state.networkSimulation.enabled);
    ImGui::SliderInt("Artificial latency / lag",&state.networkSimulation.latencyMs,0,2000,"%d ms");
    ImGui::SliderInt("Jitter (+/-)",&state.networkSimulation.jitterMs,0,1000,"%d ms");
    ImGui::SliderFloat("Packet loss",&state.networkSimulation.packetLossPercent,0.0f,100.0f,"%.1f%%");
    ImGui::SliderInt("Burst loss size",&state.networkSimulation.burstLossPackets,1,10,"%d packets");
    ImGui::SliderFloat("Duplicate packets",&state.networkSimulation.duplicatePercent,0.0f,25.0f,"%.1f%%");
    ImGui::SliderFloat("Reorder packets",&state.networkSimulation.reorderPercent,0.0f,50.0f,"%.1f%%");

    if(ImGui::Button("Reset network simulation")) state.networkSimulation={};
    if(state.client) state.client->setNetworkSimulation(state.networkSimulation);

    ImGui::Spacing();
    ImGui::TextDisabled("Simulation affects outgoing voice only. Reordering holds selected packets back by at least two voice frames.");
    ImGui::TextDisabled("Debug settings reset when the application restarts.");
}

#ifdef MKWVC_HAS_ICE
void renderIceDebug(UiState& state) {
    ImGui::TextUnformatted("ICE transport");
    ImGui::Separator();
    ImGui::Spacing();

    if(state.connectionMode==1) {
        ImGui::Text("ICE servers from signaling: %zu",state.iceServers.size());
        ImGui::Text("TURN fallback: %s",hasTurnServer(state.iceServers) ? "Available" : "Not provided");
        ImGui::BeginDisabled(state.client!=nullptr || state.iceSetup!=nullptr);
        ImGui::Checkbox("Force TURN relay (test)",&state.forceRelay);
        ImGui::EndDisabled();
    } else {
        ImGui::TextDisabled("TURN is configured automatically in Room code mode.");
    }
}
#endif

#ifdef MKWVC_HAS_ICE
void renderPeerDiagnostics(UiState& state) {
    ImGui::TextUnformatted("Per-peer diagnostics");
    ImGui::Separator();
    ImGui::Spacing();

    if(!state.client) {
        ImGui::TextDisabled("No active voice session.");
        return;
    }

    const auto peers=state.client->peerStats();
    if(peers.empty()) {
        ImGui::TextDisabled("No connected voice peers.");
        return;
    }

    const auto labelFor=[&](const mkwvc::PeerVoiceStats& stats) {
        for(const auto& peer:state.roomPeers) {
            if(peer.memberId!=stats.memberId) continue;
            return peer.name+" ["+peer.country+"]";
        }
        if(stats.memberId=="legacy") return std::string("Legacy peer");
        return stats.memberId;
    };

    for(const auto& peer:peers) {
        const auto label=labelFor(peer)+"##PeerDiag"+peer.memberId;
        if(!ImGui::TreeNode(label.c_str())) continue;

        if(ImGui::BeginTable(
            ("PeerDiagTable##"+peer.memberId).c_str(),
            2,
            ImGuiTableFlags_SizingStretchProp|ImGuiTableFlags_RowBg
        )) {
            const auto rowU64=[](const char* name,std::uint64_t value) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(name);
                ImGui::TableNextColumn();
                ImGui::Text("%llu",static_cast<unsigned long long>(value));
            };
            const auto rowText=[](const char* name,const char* value) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(name);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(value);
            };

            rowText("Receiver",peer.receiverRunning ? "Running" : "Stopped");

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted("Volume");
            ImGui::TableNextColumn();
            ImGui::Text("%.0f%%",static_cast<double>(peer.volume)*100.0);

            rowU64("TX packets",peer.txPackets);
            rowU64("RX packets",peer.rxPackets);
            rowU64("TX bytes",peer.txBytes);
            rowU64("RX bytes",peer.rxBytes);
            rowU64("Raw RX packets",peer.rawRxPackets);
            rowU64("Protocol mismatches",peer.protocolMismatchPackets);
            rowU64("Reordered RX packets",peer.reorderedRxPackets);
            rowU64("Late / duplicate RX",peer.latePackets);
            rowU64("PLC frames",peer.plcFrames);
            rowU64("FEC attempts",peer.fecAttempts);
            rowU64("Decoder errors",peer.decoderErrors);
            rowU64("Sequence resyncs",peer.sequenceResyncs);
            rowU64("Stream restarts",peer.streamRestarts);
            rowU64("Jitter buffer expansions",peer.jitterBufferExpansions);
            rowU64("Simulated drops",peer.simulatedDrops);
            rowU64("Simulated duplicates",peer.simulatedDuplicates);
            rowU64("Simulated reorders",peer.simulatedReorders);
            rowU64("Simulation queue depth",peer.simulationQueueDepth);
            rowU64("Jitter queued packets",peer.jitterBufferQueuedPackets);
            rowU64("Jitter target delay (ms)",peer.jitterBufferTargetMs);
            rowU64("Jitter current delay (ms)",peer.jitterBufferCurrentMs);

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted("Estimated jitter");
            ImGui::TableNextColumn();
            ImGui::Text("%.2f ms",static_cast<double>(peer.estimatedJitterMs));

            ImGui::EndTable();
        }

        ImGui::TreePop();
        ImGui::Spacing();
    }
}
#endif

void renderDebugStats(const mkwvc::VoiceStats& stats) {
    ImGui::TextUnformatted("Diagnostics");
    ImGui::Separator();
    ImGui::Spacing();

    if(ImGui::BeginTable("DebugStats",2,ImGuiTableFlags_SizingStretchProp|ImGuiTableFlags_RowBg)) {
        const auto row=[](const char* label,std::uint64_t value) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(label);
            ImGui::TableNextColumn();
            ImGui::Text("%llu",static_cast<unsigned long long>(value));
        };

        row("Raw transport packets received",stats.rawRxPackets);
        row("Protocol mismatches",stats.protocolMismatchPackets);
        row("Simulated drops",stats.simulatedDrops);
        row("Simulated duplicates",stats.simulatedDuplicates);
        row("Simulated reorders",stats.simulatedReorders);
        row("Reordered RX packets",stats.reorderedRxPackets);
        row("Late / duplicate RX packets",stats.latePackets);
        row("Jitter buffer expansions",stats.jitterBufferExpansions);
        row("PLC frames",stats.plcFrames);
        row("FEC attempts",stats.fecAttempts);
        row("Decoder errors",stats.decoderErrors);
        row("Sequence resyncs",stats.sequenceResyncs);
        row("Peer stream restarts",stats.streamRestarts);
        row("Simulation queue depth",stats.simulationQueueDepth);
        row("Jitter buffer queued packets",stats.jitterBufferQueuedPackets);
        row("Jitter target delay (ms)",stats.jitterBufferTargetMs);
        row("Jitter current delay (ms)",stats.jitterBufferCurrentMs);
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Estimated network jitter: %.2f ms",stats.estimatedJitterMs);
}

void renderNetworkTab(UiState& state,const mkwvc::VoiceStats& stats) {
    renderCodecDebug(state);
    ImGui::Spacing();
    ImGui::Spacing();
#ifdef MKWVC_HAS_ICE
    renderIceDebug(state);
    ImGui::Spacing();
    ImGui::Spacing();
#endif
    renderNetworkSimulation(state);
    ImGui::Spacing();
    ImGui::Spacing();
    renderDebugStats(stats);
#ifdef MKWVC_HAS_ICE
    ImGui::Spacing();
    ImGui::Spacing();
    renderPeerDiagnostics(state);
#endif
}

#ifdef MKWVC_HAS_ICE
void renderRetroRewindTab(UiState& state) {
    ImGui::TextUnformatted("Retro Rewind debug");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextDisabled("Development bridge for RR identity and authoritative room discovery.");
    ImGui::TextDisabled("The final WiiCompiled adapter will fill these credentials automatically.");

    ImGui::Spacing();
    ImGui::TextUnformatted("GPCM identity");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextUnformatted("Profile ID");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText(
        "##RRProfileId",
        state.rrProfileId.data(),
        state.rrProfileId.size(),
        ImGuiInputTextFlags_CharsDecimal
    );

    ImGui::TextUnformatted("Session key");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText(
        "##RRSessionKey",
        state.rrSessionKey.data(),
        state.rrSessionKey.size(),
        ImGuiInputTextFlags_Password
    );
    ImGui::TextDisabled("Never saved or logged by MKW VoiceChat.");

    ImGui::TextUnformatted("Game name");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##RRGameName",state.rrGameName.data(),state.rrGameName.size());

    const bool profileReady=state.rrProfileId[0]!='\0';

    ImGui::Spacing();
    ImGui::BeginDisabled(!profileReady || state.rrAuthPending);
    if(ImGui::Button("Resolve public RR room (UNVERIFIED)",ImVec2(300.0f,38.0f))) {
        try {
            if(!ensureSignaling(state)) throw std::runtime_error("Could not open signaling connection.");
            state.signaling->debugLookupRetroRewind(state.rrProfileId.data());
            state.rrAuthPending=true;
            state.rrAuthenticated=false;
            state.rrAuthStatus="Looking up PID in the public RWFC room roster...";
            state.error.clear();
        } catch(const std::exception& error) {
            state.rrAuthPending=false;
            state.rrAuthStatus=error.what();
        }
    }
    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::TextWrapped(
        "Secure identity verification is currently blocked: RR GPSP TCP port 29901 is not publicly reachable. "
        "The lookup above is DEBUG ONLY and must never authorize voice membership."
    );

    ImGui::Spacing();
    ImGui::Text("Identity: %s",state.rrAuthenticated ? "VERIFIED" : (state.rrAuthPending ? "CHECKING" : "UNVERIFIED"));
    ImGui::TextWrapped("Status: %s",state.rrAuthStatus.c_str());
    ImGui::Text("Profile ID lookup: %s",state.rrVerifiedProfileId.empty() ? "-" : state.rrVerifiedProfileId.c_str());
    ImGui::Text("Credential source: %s","Manual debug input (WiiCompiled hook pending)");

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::TextUnformatted("Authoritative RWFC room");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("Room ID: %s",state.rrRoomId.empty() ? "-" : state.rrRoomId.c_str());
    ImGui::Text("Created: %s",state.rrRoomCreated.empty() ? "-" : state.rrRoomCreated.c_str());
    ImGui::TextWrapped(
        "Room instance: %s",
        state.rrRoomInstanceId.empty() ? "-" : state.rrRoomInstanceId.c_str()
    );
    ImGui::Text("Players in MKW room: %zu",state.rrPlayers.size());

    const auto voiceUsers=static_cast<std::size_t>(std::count_if(
        state.rrPlayers.begin(),
        state.rrPlayers.end(),
        [](const RetroRewindPlayerInfo& player){return player.voiceChat;}
    ));
    ImGui::Text("VoiceChat users detected: %zu",voiceUsers);

    if(state.rrLastSync.time_since_epoch().count()!=0) {
        const auto age=std::chrono::duration<double>(
            std::chrono::steady_clock::now()-state.rrLastSync
        ).count();
        ImGui::TextDisabled("Last room sync: %.1f s ago",age);
    }

    ImGui::Spacing();
    if(state.rrPlayers.empty()) {
        ImGui::TextDisabled(
            state.rrAuthenticated
                ? "Verified session is not currently present in an active RWFC room."
                : "Enter a PID and resolve the public RR roster for debug room discovery."
        );
    } else if(ImGui::BeginTable(
        "RRRoomRoster",
        3,
        ImGuiTableFlags_SizingStretchProp|ImGuiTableFlags_RowBg|ImGuiTableFlags_BordersInnerV
    )) {
        ImGui::TableSetupColumn("Player",ImGuiTableColumnFlags_WidthStretch,1.5f);
        ImGui::TableSetupColumn("PID",ImGuiTableColumnFlags_WidthStretch,1.0f);
        ImGui::TableSetupColumn("VoiceChat",ImGuiTableColumnFlags_WidthFixed,90.0f);
        ImGui::TableHeadersRow();

        for(const auto& player:state.rrPlayers) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(player.name.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(player.participantId.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(player.voiceChat ? "YES" : "No");
        }

        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Players without VoiceChat create no signaling WebSocket, ICE connection, heartbeat or voice traffic.");
}
#endif

void renderUi(UiState& state) {
#ifdef MKWVC_HAS_ICE
    recoverTimedOutSignalingOpen(state);
    processSignalingReconnect(state);
    flushRoomState(state);
    recoverTimedOutRoomRequest(state);
    pollRoomSignaling(state);
    pollIce(state);
#endif

    const auto& io=ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0.0f,0.0f));
    ImGui::SetNextWindowSize(io.DisplaySize);

    constexpr ImGuiWindowFlags flags=ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoCollapse|ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin("MKW VoiceChat",nullptr,flags);

    ImGui::TextUnformatted("MKW VoiceChat");
    ImGui::TextDisabled("Native voice client");
    ImGui::Spacing();

    updateLiveVoiceControls(state);

    mkwvc::VoiceStats stats{};
    if(state.client) stats=state.client->stats();
    const bool active=stats.running;
    updateRates(state,stats);

    if(ImGui::BeginTabBar("MainTabs")) {
        if(ImGui::BeginTabItem("Interface")) {
            renderInterfaceTab(state,stats);
            ImGui::EndTabItem();
        }

        if(ImGui::BeginTabItem("Voice")) {
            renderVoiceTab(state,stats,active);
            ImGui::EndTabItem();
        }

        if(ImGui::BeginTabItem("Network")) {
            renderNetworkTab(state,stats);
            ImGui::EndTabItem();
        }

#ifdef MKWVC_HAS_ICE
        if(ImGui::BeginTabItem("Retro Rewind")) {
            renderRetroRewindTab(state);
            ImGui::EndTabItem();
        }
#endif

        ImGui::EndTabBar();
    }

    ImGui::End();
}

float initialUiScale() {
#ifdef _WIN32
    GLFWmonitor* monitor=glfwGetPrimaryMonitor();
    if(!monitor) return 1.0f;
    float xScale=1.0f;
    float yScale=1.0f;
    glfwGetMonitorContentScale(monitor,&xScale,&yScale);
    return std::clamp(xScale,1.0f,2.5f);
#else
    return 1.0f;
#endif
}

int runApp() {
    if(!glfwInit()) return 1;

#ifdef __APPLE__
    const char* glslVersion="#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR,3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR,2);
    glfwWindowHint(GLFW_OPENGL_PROFILE,GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT,GL_TRUE);
#else
    const char* glslVersion="#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR,3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR,0);
#endif

    const float uiScale=initialUiScale();
    const int initialWidth=static_cast<int>(760.0f*uiScale);
    const int initialHeight=static_cast<int>(760.0f*uiScale);
    const int minimumWidth=static_cast<int>(680.0f*uiScale);
    const int minimumHeight=static_cast<int>(620.0f*uiScale);

    GLFWwindow* window=glfwCreateWindow(initialWidth,initialHeight,"MKW VoiceChat",nullptr,nullptr);
    if(!window) {
        glfwTerminate();
        return 1;
    }

    glfwSetWindowSizeLimits(window,minimumWidth,minimumHeight,GLFW_DONT_CARE,GLFW_DONT_CARE);
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    auto& io=ImGui::GetIO();
    ImFontConfig fontConfig;
    fontConfig.SizePixels=16.0f*uiScale;
    io.Fonts->AddFontDefault(&fontConfig);

    ImGui::StyleColorsDark();
    auto& style=ImGui::GetStyle();
    style.ScaleAllSizes(uiScale);
    style.WindowPadding=ImVec2(18.0f*uiScale,16.0f*uiScale);
    style.FrameRounding=6.0f*uiScale;
    style.GrabRounding=6.0f*uiScale;
    style.WindowRounding=0.0f;
    style.ItemSpacing=ImVec2(10.0f*uiScale,8.0f*uiScale);

    ImGui_ImplGlfw_InitForOpenGL(window,true);
    ImGui_ImplOpenGL3_Init(glslVersion);

    UiState state;
    loadState(state);

    while(!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        renderUi(state);
        ImGui::Render();

        int width=0;
        int height=0;
        glfwGetFramebufferSize(window,&width,&height);
        glViewport(0,0,width,height);
        glClearColor(0.055f,0.06f,0.075f,1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    saveState(state);
#ifdef MKWVC_HAS_ICE
    resetIce(state);
#else
    stopClient(state);
#endif
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

}

#ifdef _WIN32
namespace {

std::vector<std::string> windowsCommandLineArgs(const char* commandLine) {
    std::vector<std::string> args{"mkw_voicechat"};
    std::istringstream input(commandLine ? commandLine : "");
    std::string arg;
    while(input>>arg) args.push_back(std::move(arg));
    return args;
}

void attachLoadTestConsole() {
    if(!AttachConsole(ATTACH_PARENT_PROCESS)) return;

    FILE* stream=nullptr;
    freopen_s(&stream,"CONOUT$","w",stdout);
    freopen_s(&stream,"CONOUT$","w",stderr);
    std::cout.clear();
    std::cerr.clear();
}

}

int WINAPI WinMain(HINSTANCE,HINSTANCE,LPSTR commandLine,int) {
    auto args=windowsCommandLineArgs(commandLine);
    if(mkwvc::loadTestRequested(args)) {
        attachLoadTestConsole();
        return mkwvc::runLoadTest(args);
    }
    return runApp();
}
#else
int main(int argc,char** argv) {
    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc));
    for(int i=0;i<argc;++i) args.emplace_back(argv[i] ? argv[i] : "");

    if(mkwvc::loadTestRequested(args)) return mkwvc::runLoadTest(args);
    return runApp();
}
#endif
