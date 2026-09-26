#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mkwvc {

struct EmbeddedCoreStatus {
    std::uint32_t sampleRate = 0;
    std::uint32_t frameDurationMs = 0;
    std::uint32_t frameSamples = 0;
    bool voiceClientCompiled = false;
    bool audioDependenciesLinked = false;
    bool iceDependenciesLinked = false;
};

struct EmbeddedVoiceSessionInput {
    bool localRoomActive = false;
    std::string profileId;
    std::string sessionKey;
    std::string gameName;
    std::vector<std::string> friendProfileIds;
    std::vector<std::string> roomProfileIds;
    bool teamModeActive = false;
    std::vector<std::string> teamProfileIds;
    std::vector<std::string> teammateProfileIds;
    std::uint64_t identityGeneration = 0;
};

struct EmbeddedVoiceRoomPlayer {
    std::string profileId;
    std::string displayName;
    std::string friendCode;
    bool voiceChat = false;
};

struct EmbeddedVoiceSessionStatus {
    bool lifecycleActive = false;
    bool signalingConnected = false;
    bool productionAuthorizationPending = false;
    bool productionAuthorized = false;
    bool developmentAdmissionPending = false;
    bool developmentAdmitted = false;
    bool voiceClientRunning = false;
    bool roomFound = false;
    std::uint32_t developmentPeerCount = 0;
    std::uint32_t peerCount = 0;
    std::string roomId;
    std::string roomInstanceId;
    std::string roomCreated;
    std::string localMemberId;
    std::vector<EmbeddedVoiceRoomPlayer> roomPlayers;
    std::string status;
};

enum class EmbeddedVoiceBindingAction : std::uint8_t {
    PushToTalk,
    PushToMute,
    ToggleMute,
    ToggleDeafen
};

struct EmbeddedVoiceBinding {
    std::int32_t keyboard = -1;
    std::string controller;
};

struct EmbeddedVoicePeerControl {
    std::string memberId;
    std::string participantId;
    std::string displayName;
    std::string friendCode;
    float volume = 1.0f;
    std::uint32_t voicePeak = 0;
    bool speaking = false;
    bool isFriend = false;
    bool isTeammate = false;
    bool policyMuted = false;
    bool remoteMuted = false;
    bool remoteDeafened = false;
};

struct EmbeddedVoiceControls {
    bool enabled = false;
    bool runtimeBlocked = false;
    std::string runtimeBlockReason;
    bool overlayVisible = true;
    bool localStatusOverlayVisible = true;
    bool playerSpeakersOverlayVisible = true;
    float playerSpeakerBackgroundTransparency = 0.30f;
    bool muteEveryone = false;
    bool muteOnlyFriends = false;
    bool muteEveryoneButFriends = false;
    bool muteTeammates = false;
    bool muteEveryoneButTeammates = false;
    bool muteNewPlayers = false;
    bool teamModeActive = false;
    std::vector<std::string> inputDevices;
    std::vector<std::string> outputDevices;
    std::string inputDevice;
    std::string outputDevice;
    bool automaticNormalization = true;
    bool noiseSuppression = true;
    int noiseSuppressionStrength = 50;
    bool noiseGate = false;
    int noiseGateThreshold = 25;
    float microphoneBoost = 1.0f;
    bool compressor = false;
    int compressorStrength = 50;
    float microphoneGain = 1.0f;
    float playbackVolume = 1.0f;
    bool microphoneMuted = false;
    bool deafened = false;
    bool pushToTalk = false;
    bool pushToMute = false;
    bool voiceActivation = false;
    int voiceActivationThreshold = 25;
    bool microphoneTest = false;
    bool pushToTalkHeld = false;
    bool pushToMuteHeld = false;
    bool localSpeaking = false;
    std::uint32_t micPeak = 0;
    std::uint32_t playbackPeak = 0;
    EmbeddedVoiceBinding pushToTalkBinding;
    EmbeddedVoiceBinding pushToMuteBinding;
    EmbeddedVoiceBinding muteBinding;
    EmbeddedVoiceBinding deafenBinding;
    std::vector<EmbeddedVoicePeerControl> peers;
    std::string error;
};

EmbeddedCoreStatus embeddedCoreStatus() noexcept;
void serviceEmbeddedVoiceSession(const EmbeddedVoiceSessionInput& input) noexcept;
EmbeddedVoiceSessionStatus embeddedVoiceSessionStatus();
EmbeddedVoiceControls embeddedVoiceControls();
void setEmbeddedVoiceRuntimeBlocked(bool blocked,std::string reason);
void setEmbeddedVoiceEnabled(bool enabled);
void setEmbeddedVoiceOverlayVisible(bool visible);
void setEmbeddedVoiceOverlayOptions(bool localStatusVisible,bool playerSpeakersVisible,float playerSpeakerBackgroundTransparency);
void setEmbeddedVoiceMutePolicy(bool muteEveryone,bool muteOnlyFriends,bool muteEveryoneButFriends,bool muteTeammates,bool muteEveryoneButTeammates,bool muteNewPlayers);
void refreshEmbeddedVoiceDevices();
void setEmbeddedVoiceInputDevice(std::string device);
void setEmbeddedVoiceOutputDevice(std::string device);
void setEmbeddedVoiceProcessing(bool normalization,bool noiseSuppression,int noiseSuppressionStrength);
void setEmbeddedVoiceAdvancedProcessing(bool noiseGate,int noiseGateThreshold,float microphoneBoost,bool compressor,int compressorStrength);
void setEmbeddedVoiceMicrophoneGain(float gain);
void setEmbeddedVoicePlaybackVolume(float volume);
void setEmbeddedVoiceMicrophoneMuted(bool muted);
void setEmbeddedVoiceDeafened(bool deafened);
void setEmbeddedVoicePushToTalk(bool enabled);
void setEmbeddedVoicePushToMute(bool enabled);
void setEmbeddedVoiceVoiceActivation(bool enabled,int threshold);
void setEmbeddedVoiceHotkeyState(bool pushToTalkHeld,bool pushToMuteHeld);
void setEmbeddedVoiceBinding(EmbeddedVoiceBindingAction action,std::int32_t keyboard,std::string controller);
void setEmbeddedVoiceMicrophoneTest(bool enabled);
void setEmbeddedVoicePeerVolume(const std::string& memberId,float volume);
void resetEmbeddedVoiceAudioSettings();
void playEmbeddedVoiceTestTone();

}
