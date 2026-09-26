#include "mkwvc/SignalingClient.hpp"

#include <rtc/rtc.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace mkwvc {

namespace {

std::string encodeHex(std::string_view value) {
    constexpr char hex[]="0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(value.size()*2);
    for(const unsigned char ch:value) {
        encoded.push_back(hex[ch>>4]);
        encoded.push_back(hex[ch&0x0F]);
    }
    return encoded;
}

}

class SignalingClient::Impl {
public:
    explicit Impl(std::string serverUrl) {
        if(serverUrl.empty()) throw std::invalid_argument("Signaling server URL is empty");

        rtc::WebSocket::Configuration config;
        config.connectionTimeout=std::chrono::seconds(5);
        config.pingInterval=std::chrono::seconds(3);
        config.maxOutstandingPings=2;
        socket_=std::make_shared<rtc::WebSocket>(std::move(config));

        socket_->onOpen([this] {
            std::vector<std::string> pending;
            std::shared_ptr<rtc::WebSocket> socket;
            {
                std::scoped_lock lock(mutex_);
                if(closed_) return;
                open_=true;
                roomHeartbeatEnabled_=false;
                rrHeartbeatEnabled_=false;
                nextHeartbeat_=std::chrono::steady_clock::now()+std::chrono::seconds(60);
                socket=socket_;
                pending.swap(pending_);
                events_.push_back({SignalingEventType::Open,{}});
            }

            for(const auto& message:pending) {
                if(!socket->send(message)) pushEvent(SignalingEventType::Error,"Failed to send signaling message");
            }
        });

        socket_->onMessage([this](rtc::message_variant message) {
            const auto* text=std::get_if<std::string>(&message);
            if(!text) return;
            handleMessage(*text);
        });

        socket_->onError([this](std::string error) {
            pushEvent(SignalingEventType::TransportError,std::move(error));
        });

        socket_->onClosed([this] {
            std::scoped_lock lock(mutex_);
            open_=false;
            closed_=true;
            events_.push_back({SignalingEventType::Closed,{}});
        });

        socket_->open(serverUrl);
    }

    ~Impl() {
        std::shared_ptr<rtc::WebSocket> socket;
        {
            std::scoped_lock lock(mutex_);
            closed_=true;
            open_=false;
            socket=std::move(socket_);
            pending_.clear();
        }

        if(socket) {
            socket->resetCallbacks();
            socket->close();
        }
    }

    void setRoomNone() {
        sendCommand("LEAVE");
    }

    void setRoomCreate(std::string memberId,std::string displayName) {
        memberId.erase(
            std::remove_if(memberId.begin(),memberId.end(),[](unsigned char ch){return std::isspace(ch)!=0;}),
            memberId.end()
        );
        std::transform(memberId.begin(),memberId.end(),memberId.begin(),[](unsigned char ch){
            return static_cast<char>(std::toupper(ch));
        });
        if(memberId.size()!=32 ||
           !std::all_of(memberId.begin(),memberId.end(),[](unsigned char ch){return std::isxdigit(ch)!=0;})) {
            throw std::invalid_argument("Invalid signaling member ID");
        }
        if(displayName.empty()) displayName="Player";
        if(displayName.size()>63) displayName.resize(63);
        sendCommand("CREATE "+memberId+" "+encodeHex(displayName));
    }

    void setRoomJoin(std::string roomCode,std::string memberId,std::string displayName) {
        roomCode.erase(
            std::remove_if(roomCode.begin(),roomCode.end(),[](unsigned char ch){return std::isspace(ch)!=0;}),
            roomCode.end()
        );
        std::transform(roomCode.begin(),roomCode.end(),roomCode.begin(),[](unsigned char ch){
            return static_cast<char>(std::toupper(ch));
        });
        memberId.erase(
            std::remove_if(memberId.begin(),memberId.end(),[](unsigned char ch){return std::isspace(ch)!=0;}),
            memberId.end()
        );
        std::transform(memberId.begin(),memberId.end(),memberId.begin(),[](unsigned char ch){
            return static_cast<char>(std::toupper(ch));
        });
        if(roomCode.empty()) throw std::invalid_argument("Room code is empty");
        if(memberId.size()!=32 ||
           !std::all_of(memberId.begin(),memberId.end(),[](unsigned char ch){return std::isxdigit(ch)!=0;})) {
            throw std::invalid_argument("Invalid signaling member ID");
        }
        if(displayName.empty()) displayName="Player";
        if(displayName.size()>63) displayName.resize(63);
        sendCommand("JOIN "+roomCode+" "+memberId+" "+encodeHex(displayName));
    }

    void sendSignal(std::string signal) {
        if(signal.empty()) throw std::invalid_argument("ICE signal is empty");
        sendCommand("SIGNAL\n"+signal);
    }

    void sendSignal(std::string memberId,std::string signal) {
        memberId.erase(
            std::remove_if(memberId.begin(),memberId.end(),[](unsigned char ch){return std::isspace(ch)!=0;}),
            memberId.end()
        );
        std::transform(memberId.begin(),memberId.end(),memberId.begin(),[](unsigned char ch){
            return static_cast<char>(std::toupper(ch));
        });
        if(memberId.size()!=32 ||
           !std::all_of(memberId.begin(),memberId.end(),[](unsigned char ch){return std::isxdigit(ch)!=0;})) {
            throw std::invalid_argument("Invalid signaling target member ID");
        }
        if(signal.empty()) throw std::invalid_argument("ICE signal is empty");
        sendCommand("SIGNAL "+memberId+"\n"+signal);
    }

    void authenticateRetroRewind(std::string profileId,std::string sessionKey,std::string gameName) {
        const auto trim=[](std::string& value) {
            value.erase(
                std::remove_if(value.begin(),value.end(),[](unsigned char ch){return std::isspace(ch)!=0;}),
                value.end()
            );
        };

        trim(profileId);
        trim(sessionKey);
        trim(gameName);

        const bool validProfile=
            !profileId.empty() &&
            profileId.size()<=10 &&
            std::all_of(profileId.begin(),profileId.end(),[](unsigned char ch){return std::isdigit(ch)!=0;});
        const bool validSession=
            !sessionKey.empty() &&
            sessionKey.size()<=11 &&
            std::all_of(
                sessionKey.begin()+(sessionKey.front()=='-' ? 1 : 0),
                sessionKey.end(),
                [](unsigned char ch){return std::isdigit(ch)!=0;}
            ) &&
            !(sessionKey=="-");
        const bool validGame=
            !gameName.empty() &&
            gameName.size()<=32 &&
            std::all_of(gameName.begin(),gameName.end(),[](unsigned char ch){
                return std::isalnum(ch)!=0 || ch=='_' || ch=='-';
            });

        if(!validProfile) throw std::invalid_argument("Retro Rewind profile ID must be a decimal uint32 value");
        if(!validSession) throw std::invalid_argument("Retro Rewind session key must be a decimal int32 value");
        if(!validGame) throw std::invalid_argument("Retro Rewind game name is invalid");

        sendCommand("RR_AUTH "+profileId+" "+sessionKey+" "+gameName);
    }

    void admitRetroRewindRoom(std::string roomInstanceId) {
        roomInstanceId.erase(
            std::remove_if(roomInstanceId.begin(),roomInstanceId.end(),[](unsigned char ch){return std::isspace(ch)!=0;}),
            roomInstanceId.end()
        );
        std::transform(roomInstanceId.begin(),roomInstanceId.end(),roomInstanceId.begin(),[](unsigned char ch){
            return static_cast<char>(std::toupper(ch));
        });
        if(roomInstanceId.size()!=64 ||
           !std::all_of(roomInstanceId.begin(),roomInstanceId.end(),[](unsigned char ch){return std::isxdigit(ch)!=0;})) {
            throw std::invalid_argument("Retro Rewind room instance is invalid");
        }
        sendCommand("RR_ADMIT "+roomInstanceId);
    }

    void admitRetroRewindDevelopment(
        std::string profileId,
        std::vector<std::string> localProfileIds) {
        profileId.erase(
            std::remove_if(profileId.begin(),profileId.end(),[](unsigned char ch){return std::isspace(ch)!=0;}),
            profileId.end()
        );

        const auto validProfile=[](const std::string& value) {
            return
                !value.empty() &&
                value.size()<=10 &&
                std::all_of(value.begin(),value.end(),[](unsigned char ch){return std::isdigit(ch)!=0;});
        };

        if(!validProfile(profileId)) {
            throw std::invalid_argument("Retro Rewind profile ID must be a decimal uint32 value");
        }

        for(auto& value:localProfileIds) {
            value.erase(
                std::remove_if(value.begin(),value.end(),[](unsigned char ch){return std::isspace(ch)!=0;}),
                value.end());
        }
        localProfileIds.erase(
            std::remove_if(
                localProfileIds.begin(),
                localProfileIds.end(),
                [&](const std::string& value) {
                    return !validProfile(value);
                }),
            localProfileIds.end());

        localProfileIds.push_back(profileId);
        std::sort(localProfileIds.begin(),localProfileIds.end());
        localProfileIds.erase(
            std::unique(localProfileIds.begin(),localProfileIds.end()),
            localProfileIds.end());
        if(localProfileIds.size()>12) localProfileIds.resize(12);

        std::string command="RR_DEV_ADMIT "+profileId;
        if(!localProfileIds.empty()) {
            command+=" PIDS=";
            for(std::size_t i=0;i<localProfileIds.size();++i) {
                if(i>0) command.push_back(',');
                command+=localProfileIds[i];
            }
        }
        sendCommand(std::move(command));
    }

    void debugLookupRetroRewind(std::string profileId) {
        profileId.erase(
            std::remove_if(profileId.begin(),profileId.end(),[](unsigned char ch){return std::isspace(ch)!=0;}),
            profileId.end()
        );
        const bool validProfile=
            !profileId.empty() &&
            profileId.size()<=10 &&
            std::all_of(profileId.begin(),profileId.end(),[](unsigned char ch){return std::isdigit(ch)!=0;});
        if(!validProfile) throw std::invalid_argument("Retro Rewind profile ID must be a decimal uint32 value");
        sendCommand("RR_DEBUG_LOOKUP "+profileId);
    }

    void setDebugRetroRewindPresence(std::string profileId) {
        profileId.erase(
            std::remove_if(profileId.begin(),profileId.end(),[](unsigned char ch){return std::isspace(ch)!=0;}),
            profileId.end()
        );
        const bool validProfile=
            !profileId.empty() &&
            profileId.size()<=10 &&
            std::all_of(profileId.begin(),profileId.end(),[](unsigned char ch){return std::isdigit(ch)!=0;});
        if(!validProfile) throw std::invalid_argument("Retro Rewind profile ID must be a decimal uint32 value");
        sendCommand("RR_DEBUG_PRESENCE "+profileId);
    }

    void clearDebugRetroRewindPresence() {
        sendCommand("RR_DEBUG_PRESENCE_CLEAR");
    }

    void syncRetroRewindRoom() {
        sendCommand("RR_SYNC");
    }

    bool open() const {
        std::scoped_lock lock(mutex_);
        return open_;
    }

    void service() {
        std::shared_ptr<rtc::WebSocket> socket;
        {
            std::scoped_lock lock(mutex_);
            const auto now=std::chrono::steady_clock::now();
            if(closed_ || !open_ ||
               (!roomHeartbeatEnabled_ && !rrHeartbeatEnabled_) ||
               now<nextHeartbeat_) {
                return;
            }
            nextHeartbeat_=now+std::chrono::seconds(60);
            socket=socket_;
        }

        if(socket && !socket->send("ALIVE")) {
            pushEvent(SignalingEventType::TransportError,"Signaling heartbeat send failed");
        }
    }

    std::vector<SignalingEvent> takeEvents() {
        std::scoped_lock lock(mutex_);
        auto events=std::move(events_);
        events_.clear();
        return events;
    }

private:
    void sendCommand(std::string message) {
        std::shared_ptr<rtc::WebSocket> socket;
        {
            std::scoped_lock lock(mutex_);
            if(closed_) throw std::runtime_error("Signaling connection is closed");
            if(!open_) {
                pending_.push_back(std::move(message));
                return;
            }
            socket=socket_;
        }

        if(!socket || !socket->send(message)) throw std::runtime_error("Failed to send signaling message");
    }

    void pushEvent(SignalingEventType type,std::string payload) {
        std::scoped_lock lock(mutex_);
        events_.push_back({type,std::move(payload)});
    }

    void handleMessage(const std::string& message) {
        if(message.starts_with("ROOM ")) {
            {
                std::scoped_lock lock(mutex_);
                roomHeartbeatEnabled_=true;
                nextHeartbeat_=std::chrono::steady_clock::now()+std::chrono::seconds(60);
            }
            pushEvent(SignalingEventType::RoomCreated,message.substr(5));
        } else if(message.starts_with("JOINED ")) {
            {
                std::scoped_lock lock(mutex_);
                roomHeartbeatEnabled_=true;
                nextHeartbeat_=std::chrono::steady_clock::now()+std::chrono::seconds(60);
            }
            pushEvent(SignalingEventType::RoomJoined,message.substr(7));
        } else if(message=="LEFT") {
            {
                std::scoped_lock lock(mutex_);
                roomHeartbeatEnabled_=false;
            }
            pushEvent(SignalingEventType::RoomLeft,{});
        } else if(message.starts_with("ICE_SERVERS\n")) {
            pushEvent(SignalingEventType::IceServers,message.substr(12));
        } else if(message=="PEER_READY") {
            pushEvent(SignalingEventType::PeerReady,{});
        } else if(message.starts_with("PEER_READY ")) {
            std::scoped_lock lock(mutex_);
            events_.push_back({SignalingEventType::PeerReady,{},message.substr(11)});
        } else if(message.starts_with("PEER_INFO\n")) {
            pushEvent(SignalingEventType::PeerInfo,message.substr(10));
        } else if(message.starts_with("SIGNAL\n")) {
            pushEvent(SignalingEventType::Signal,message.substr(7));
        } else if(message.starts_with("SIGNAL ")) {
            const auto split=message.find('\n');
            if(split==std::string::npos || split<=7) return;
            std::scoped_lock lock(mutex_);
            events_.push_back({SignalingEventType::Signal,message.substr(split+1),message.substr(7,split-7)});
        } else if(message=="PEER_LEFT") {
            pushEvent(SignalingEventType::PeerLeft,{});
        } else if(message.starts_with("PEER_LEFT ")) {
            pushEvent(SignalingEventType::PeerLeft,message.substr(10));
        } else if(message.starts_with("RR_STATUS\n")) {
            pushEvent(SignalingEventType::RetroRewindStatus,message.substr(10));
        } else if(message.starts_with("RR_DEBUG_STATUS\n")) {
            pushEvent(SignalingEventType::RetroRewindDebugStatus,message.substr(16));
        } else if(message.starts_with("RR_DEBUG_FAIL ")) {
            pushEvent(SignalingEventType::RetroRewindDebugFailed,message.substr(14));
        } else if(message.starts_with("RR_AUTH_FAIL ")) {
            pushEvent(SignalingEventType::RetroRewindAuthFailed,message.substr(13));
        } else if(message=="RR_AUTH_REQUIRED") {
            {
                std::scoped_lock lock(mutex_);
                rrHeartbeatEnabled_=false;
            }
            pushEvent(SignalingEventType::RetroRewindAuthRequired,{});
        } else if(message.starts_with("RR_ADMITTED ")) {
            {
                std::scoped_lock lock(mutex_);
                rrHeartbeatEnabled_=true;
                nextHeartbeat_=std::chrono::steady_clock::now()+std::chrono::seconds(60);
            }
            pushEvent(SignalingEventType::RetroRewindAdmitted,message.substr(12));
        } else if(message.starts_with("RR_ADMIT_FAIL ")) {
            {
                std::scoped_lock lock(mutex_);
                rrHeartbeatEnabled_=false;
            }
            pushEvent(SignalingEventType::RetroRewindAdmissionFailed,message.substr(14));
        } else if(message.starts_with("RR_PEER_INFO\n")) {
            pushEvent(SignalingEventType::RetroRewindPeerInfo,message.substr(13));
        } else if(message.starts_with("RR_DEV_ADMITTED ")) {
            {
                std::scoped_lock lock(mutex_);
                rrHeartbeatEnabled_=true;
                nextHeartbeat_=std::chrono::steady_clock::now()+std::chrono::seconds(60);
            }
            pushEvent(SignalingEventType::RetroRewindDevelopmentAdmitted,message.substr(16));
        } else if(message.starts_with("RR_DEV_ADMIT_FAIL ")) {
            {
                std::scoped_lock lock(mutex_);
                rrHeartbeatEnabled_=false;
            }
            pushEvent(SignalingEventType::RetroRewindDevelopmentAdmissionFailed,message.substr(18));
        } else if(message.starts_with("RR_DEV_PEER_INFO\n")) {
            pushEvent(SignalingEventType::RetroRewindDevelopmentPeerInfo,message.substr(17));
        } else if(message.starts_with("ERROR ")) {
            pushEvent(SignalingEventType::Error,message.substr(6));
        }
    }

    mutable std::mutex mutex_;
    std::shared_ptr<rtc::WebSocket> socket_;
    std::vector<std::string> pending_;
    std::vector<SignalingEvent> events_;
    bool open_=false;
    bool closed_=false;
    bool roomHeartbeatEnabled_=false;
    bool rrHeartbeatEnabled_=false;
    std::chrono::steady_clock::time_point nextHeartbeat_{};
};

SignalingClient::SignalingClient(std::string serverUrl):impl_(std::make_unique<Impl>(std::move(serverUrl))) {}
SignalingClient::~SignalingClient()=default;
void SignalingClient::setRoomNone(){impl_->setRoomNone();}
void SignalingClient::setRoomCreate(std::string memberId,std::string displayName){impl_->setRoomCreate(std::move(memberId),std::move(displayName));}
void SignalingClient::setRoomJoin(std::string roomCode,std::string memberId,std::string displayName){impl_->setRoomJoin(std::move(roomCode),std::move(memberId),std::move(displayName));}
void SignalingClient::sendSignal(std::string signal){impl_->sendSignal(std::move(signal));}
void SignalingClient::sendSignal(std::string memberId,std::string signal){impl_->sendSignal(std::move(memberId),std::move(signal));}
void SignalingClient::authenticateRetroRewind(std::string profileId,std::string sessionKey,std::string gameName){impl_->authenticateRetroRewind(std::move(profileId),std::move(sessionKey),std::move(gameName));}
void SignalingClient::admitRetroRewindRoom(std::string roomInstanceId){impl_->admitRetroRewindRoom(std::move(roomInstanceId));}
void SignalingClient::admitRetroRewindDevelopment(std::string profileId,std::vector<std::string> localProfileIds){impl_->admitRetroRewindDevelopment(std::move(profileId),std::move(localProfileIds));}
void SignalingClient::debugLookupRetroRewind(std::string profileId){impl_->debugLookupRetroRewind(std::move(profileId));}
void SignalingClient::setDebugRetroRewindPresence(std::string profileId){impl_->setDebugRetroRewindPresence(std::move(profileId));}
void SignalingClient::clearDebugRetroRewindPresence(){impl_->clearDebugRetroRewindPresence();}
void SignalingClient::syncRetroRewindRoom(){impl_->syncRetroRewindRoom();}
bool SignalingClient::open() const{return impl_->open();}
void SignalingClient::service(){impl_->service();}
std::vector<SignalingEvent> SignalingClient::takeEvents(){return impl_->takeEvents();}

}
