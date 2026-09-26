#pragma once

#include <memory>
#include <string>
#include <vector>

namespace mkwvc {

enum class SignalingEventType {
    Open,
    RoomCreated,
    RoomJoined,
    RoomLeft,
    IceServers,
    PeerReady,
    PeerInfo,
    Signal,
    PeerLeft,
    RetroRewindStatus,
    RetroRewindDebugStatus,
    RetroRewindDebugFailed,
    RetroRewindAuthFailed,
    RetroRewindAuthRequired,
    RetroRewindAdmitted,
    RetroRewindAdmissionFailed,
    RetroRewindPeerInfo,
    RetroRewindDevelopmentAdmitted,
    RetroRewindDevelopmentAdmissionFailed,
    RetroRewindDevelopmentPeerInfo,
    Error,
    TransportError,
    Closed
};

struct SignalingEvent {
    SignalingEventType type;
    std::string payload;
    std::string memberId;
};

class SignalingClient {
public:
    explicit SignalingClient(std::string serverUrl);
    ~SignalingClient();

    SignalingClient(const SignalingClient&)=delete;
    SignalingClient& operator=(const SignalingClient&)=delete;

    void setRoomNone();
    void setRoomCreate(std::string memberId,std::string displayName);
    void setRoomJoin(std::string roomCode,std::string memberId,std::string displayName);
    void sendSignal(std::string signal);
    void sendSignal(std::string memberId,std::string signal);
    void authenticateRetroRewind(std::string profileId,std::string sessionKey,std::string gameName);
    void admitRetroRewindRoom(std::string roomInstanceId);
    void admitRetroRewindDevelopment(std::string profileId,std::vector<std::string> localProfileIds={});
    void debugLookupRetroRewind(std::string profileId);
    void setDebugRetroRewindPresence(std::string profileId);
    void clearDebugRetroRewindPresence();
    void syncRetroRewindRoom();
    bool open() const;
    void service();
    std::vector<SignalingEvent> takeEvents();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}
