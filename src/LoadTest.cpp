#include "mkwvc/LoadTest.hpp"

#ifdef MKWVC_HAS_ICE

#include "mkwvc/AdaptiveJitterBuffer.hpp"
#include "mkwvc/IcePeerTransport.hpp"
#include "mkwvc/IceSignal.hpp"
#include "mkwvc/OpusCodec.hpp"
#include "mkwvc/SignalingClient.hpp"
#include "mkwvc/VoiceFormat.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

namespace mkwvc {

namespace {

using Clock=std::chrono::steady_clock;
constexpr std::uint8_t PacketVersion=3;
constexpr std::string_view DefaultServer="wss://mkw-voicechat-signaling.mkwvoicechat.workers.dev";
std::uint64_t gSignalMessagesSent=0;
std::uint64_t gMaxSignalMessagesPerSession=0;

struct ProcessSnapshot {
    double cpuSeconds=0.0;
    std::uint64_t residentBytes=0;
};

struct LoadTestOptions {
    int peers=12;
    int durationSeconds=15;
    int connectTimeoutSeconds=60;
    std::string server=std::string(DefaultServer);
};

struct PeerLink {
    std::string remoteId;
    std::unique_ptr<IcePeerTransport> transport;
    std::optional<IceDescriptionSignal> localDescription;
    std::vector<IceCandidateSignal> localCandidates;
    OpusDecoderCodec decoder;
    AdaptiveJitterBuffer jitter;
    std::uint64_t streamId=0;
    bool haveStream=false;
    bool offerer=false;
    bool signalSent=false;
    std::uint64_t rxPackets=0;
    std::uint64_t rxBytes=0;
    std::uint64_t decodedFrames=0;
    std::uint64_t decoderErrors=0;
};

struct Participant {
    explicit Participant(int value):
        index(value),
        memberId(makeMemberId(value)),
        name("LoadPeer"+std::to_string(value)),
        streamId(0x4D4B570000000000ULL+static_cast<std::uint64_t>(value)) {}

    static std::string makeMemberId(int index) {
        constexpr char hex[]="0123456789ABCDEF";
        std::string id(32,'0');
        auto value=static_cast<std::uint64_t>(index);
        for(int i=31;i>=0 && value>0;--i) {
            id[static_cast<std::size_t>(i)]=hex[value&0x0F];
            value>>=4;
        }
        return id;
    }

    int index=0;
    std::string memberId;
    std::string name;
    std::unique_ptr<SignalingClient> signaling;
    std::vector<std::string> iceServers;
    std::vector<std::string> pendingReady;
    std::unordered_map<std::string,std::unique_ptr<PeerLink>> links;
    OpusEncoderCodec encoder;
    std::string roomCode;
    std::uint64_t streamId=0;
    std::uint32_t sequence=0;
    double phase=0.0;
    bool roomReady=false;
    std::uint64_t txPackets=0;
    std::uint64_t txBytes=0;
    std::uint64_t mixedFrames=0;
    std::uint64_t clippedSamples=0;
    std::uint64_t signalMessagesSent=0;
};

ProcessSnapshot processSnapshot() {
    ProcessSnapshot result;
#ifdef _WIN32
    FILETIME createTime{},exitTime{},kernelTime{},userTime{};
    if(GetProcessTimes(GetCurrentProcess(),&createTime,&exitTime,&kernelTime,&userTime)) {
        ULARGE_INTEGER kernel{};
        ULARGE_INTEGER user{};
        kernel.LowPart=kernelTime.dwLowDateTime;
        kernel.HighPart=kernelTime.dwHighDateTime;
        user.LowPart=userTime.dwLowDateTime;
        user.HighPart=userTime.dwHighDateTime;
        result.cpuSeconds=static_cast<double>(kernel.QuadPart+user.QuadPart)/10000000.0;
    }

    PROCESS_MEMORY_COUNTERS_EX memory{};
    if(GetProcessMemoryInfo(
        GetCurrentProcess(),
        reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
        sizeof(memory)
    )) {
        result.residentBytes=static_cast<std::uint64_t>(memory.PeakWorkingSetSize);
    }
#else
    rusage usage{};
    if(getrusage(RUSAGE_SELF,&usage)==0) {
        result.cpuSeconds=
            static_cast<double>(usage.ru_utime.tv_sec)+static_cast<double>(usage.ru_utime.tv_usec)/1000000.0+
            static_cast<double>(usage.ru_stime.tv_sec)+static_cast<double>(usage.ru_stime.tv_usec)/1000000.0;
#ifdef __APPLE__
        result.residentBytes=static_cast<std::uint64_t>(usage.ru_maxrss);
#else
        result.residentBytes=static_cast<std::uint64_t>(usage.ru_maxrss)*1024ULL;
#endif
    }
#endif
    return result;
}

int parseInt(std::string_view value,const char* name,int min,int max) {
    if(value.empty()) throw std::invalid_argument(std::string(name)+" is missing");
    char* end=nullptr;
    const std::string text(value);
    const long parsed=std::strtol(text.c_str(),&end,10);
    if(!end || *end!='\0' || parsed<min || parsed>max) {
        throw std::invalid_argument(
            std::string(name)+" must be between "+std::to_string(min)+" and "+std::to_string(max)
        );
    }
    return static_cast<int>(parsed);
}

LoadTestOptions parseOptions(std::span<const std::string> args) {
    LoadTestOptions options;

    for(std::size_t i=0;i<args.size();++i) {
        const auto& arg=args[i];
        if(arg=="--loadtest") continue;

        if(arg=="--peers") {
            if(i+1>=args.size()) throw std::invalid_argument("--peers requires a value");
            options.peers=parseInt(args[++i],"--peers",2,12);
        } else if(arg=="--duration") {
            if(i+1>=args.size()) throw std::invalid_argument("--duration requires a value");
            options.durationSeconds=parseInt(args[++i],"--duration",5,300);
        } else if(arg=="--connect-timeout") {
            if(i+1>=args.size()) throw std::invalid_argument("--connect-timeout requires a value");
            options.connectTimeoutSeconds=parseInt(args[++i],"--connect-timeout",10,180);
        } else if(arg=="--server") {
            if(i+1>=args.size()) throw std::invalid_argument("--server requires a value");
            options.server=args[++i];
            if(!options.server.starts_with("ws://") && !options.server.starts_with("wss://")) {
                throw std::invalid_argument("--server must be a ws:// or wss:// URL");
            }
        } else if(arg=="--help" || arg=="-h") {
            continue;
        } else if(!arg.empty() && arg[0]=='-') {
            throw std::invalid_argument("Unknown load-test option: "+arg);
        }
    }

    return options;
}

std::vector<std::string> parseIceServers(std::string_view text) {
    std::vector<std::string> servers;
    while(!text.empty()) {
        const auto end=text.find('\n');
        const auto line=text.substr(0,end);
        if(!line.empty()) servers.emplace_back(line);
        if(end==std::string_view::npos) break;
        text.remove_prefix(end+1);
    }
    if(servers.empty()) throw std::runtime_error("Signaling returned no ICE servers");
    return servers;
}

void writeStreamId(std::span<std::byte> packet,std::uint64_t streamId) {
    for(std::size_t i=0;i<8;++i) {
        packet[6+i]=static_cast<std::byte>((streamId>>(56-i*8))&0xFF);
    }
}

std::uint64_t readStreamId(std::span<const std::byte> packet) {
    std::uint64_t value=0;
    for(std::size_t i=0;i<8;++i) {
        value=(value<<8)|std::to_integer<std::uint64_t>(packet[6+i]);
    }
    return value;
}

void writeSequence(std::span<std::byte> packet,std::uint32_t sequence) {
    packet[14]=static_cast<std::byte>((sequence>>24)&0xFF);
    packet[15]=static_cast<std::byte>((sequence>>16)&0xFF);
    packet[16]=static_cast<std::byte>((sequence>>8)&0xFF);
    packet[17]=static_cast<std::byte>(sequence&0xFF);
}

std::uint32_t readSequence(std::span<const std::byte> packet) {
    return
        (std::to_integer<std::uint32_t>(packet[14])<<24)|
        (std::to_integer<std::uint32_t>(packet[15])<<16)|
        (std::to_integer<std::uint32_t>(packet[16])<<8)|
        std::to_integer<std::uint32_t>(packet[17]);
}

bool validVoicePacket(std::span<const std::byte> packet) {
    return
        packet.size()>VoiceFormat::PacketHeaderBytes &&
        packet.size()<=VoiceFormat::MaxVoicePacketBytes &&
        packet[0]==static_cast<std::byte>('M') &&
        packet[1]==static_cast<std::byte>('K') &&
        packet[2]==static_cast<std::byte>('W') &&
        packet[3]==static_cast<std::byte>('V') &&
        packet[4]==static_cast<std::byte>(PacketVersion);
}

PeerLink& ensureLink(Participant& participant,const std::string& remoteId) {
    if(const auto found=participant.links.find(remoteId);found!=participant.links.end()) {
        return *found->second;
    }

    if(participant.iceServers.empty()) {
        throw std::runtime_error(participant.name+": peer ready before ICE server configuration");
    }

    auto link=std::make_unique<PeerLink>();
    link->remoteId=remoteId;
    link->offerer=participant.memberId<remoteId;
    link->transport=std::make_unique<IcePeerTransport>(participant.iceServers,false);
    if(link->offerer) link->transport->beginOffer();

    auto* raw=link.get();
    participant.links.emplace(remoteId,std::move(link));
    return *raw;
}

void queueReady(Participant& participant,const std::string& remoteId) {
    if(remoteId.empty() || remoteId==participant.memberId) return;

    if(const auto existing=participant.links.find(remoteId);
       existing!=participant.links.end()) {
        participant.links.erase(existing);
    }

    participant.pendingReady.erase(
        std::remove(
            participant.pendingReady.begin(),
            participant.pendingReady.end(),
            remoteId),
        participant.pendingReady.end());

    if(participant.iceServers.empty()) {
        participant.pendingReady.push_back(remoteId);
        return;
    }
    ensureLink(participant,remoteId);
}

void applySignal(Participant& participant,const SignalingEvent& event) {
    if(event.memberId.empty()) return;

    const auto bundle=decodeIceSignal(event.payload);
    PeerLink* link=nullptr;

    if(const auto found=participant.links.find(event.memberId);found!=participant.links.end()) {
        link=found->second.get();
    } else {
        if(participant.iceServers.empty()) {
            throw std::runtime_error(participant.name+": ICE signal arrived before ICE server configuration");
        }
        auto created=std::make_unique<PeerLink>();
        created->remoteId=event.memberId;
        created->offerer=false;
        created->transport=std::make_unique<IcePeerTransport>(participant.iceServers,false);
        link=created.get();
        participant.links.emplace(event.memberId,std::move(created));
    }

    const char* expected=link->offerer ? "answer" : "offer";
    if(bundle.description.type!=expected) {
        throw std::runtime_error(
            participant.name+": expected ICE "+std::string(expected)+" from "+event.memberId+
            ", got "+bundle.description.type
        );
    }

    link->transport->setRemoteDescription(bundle.description.sdp,bundle.description.type);
    for(const auto& candidate:bundle.candidates) {
        link->transport->addRemoteCandidate(candidate.candidate,candidate.mid);
    }
}

void processSignaling(Participant& participant) {
    participant.signaling->service();

    for(auto& event:participant.signaling->takeEvents()) {
        switch(event.type) {
            case SignalingEventType::Open:
                break;
            case SignalingEventType::RoomCreated:
            case SignalingEventType::RoomJoined: {
                const bool changedRoom=
                    participant.roomReady &&
                    !participant.roomCode.empty() &&
                    participant.roomCode!=event.payload;
                participant.roomCode=event.payload;
                participant.roomReady=true;
                if(changedRoom) {
                    participant.links.clear();
                    participant.pendingReady.clear();
                }
                break;
            }
            case SignalingEventType::IceServers:
                participant.iceServers=parseIceServers(event.payload);
                for(const auto& remoteId:participant.pendingReady) ensureLink(participant,remoteId);
                participant.pendingReady.clear();
                break;
            case SignalingEventType::PeerReady:
                queueReady(participant,event.memberId);
                break;
            case SignalingEventType::Signal:
                applySignal(participant,event);
                break;
            case SignalingEventType::PeerInfo:
            case SignalingEventType::RetroRewindStatus:
            case SignalingEventType::RetroRewindDebugStatus:
            case SignalingEventType::RetroRewindDebugFailed:
            case SignalingEventType::RetroRewindAuthFailed:
            case SignalingEventType::RetroRewindAuthRequired:
                break;
            case SignalingEventType::PeerLeft:
                if(event.payload.empty()) {
                    throw std::runtime_error(participant.name+": unaddressed peer-left event");
                }
                participant.links.erase(event.payload);
                participant.pendingReady.erase(
                    std::remove(
                        participant.pendingReady.begin(),
                        participant.pendingReady.end(),
                        event.payload),
                    participant.pendingReady.end());
                break;
            case SignalingEventType::RoomLeft:
                throw std::runtime_error(participant.name+": room membership ended during load test");
            case SignalingEventType::Error:
                throw std::runtime_error(participant.name+": signaling error: "+event.payload);
            case SignalingEventType::TransportError:
                throw std::runtime_error(participant.name+": signaling transport error: "+event.payload);
            case SignalingEventType::Closed:
                throw std::runtime_error(participant.name+": signaling connection closed");
        }
    }

    for(auto& [remoteId,owned]:participant.links) {
        auto& link=*owned;
        if(auto description=link.transport->takeLocalDescription()) {
            link.localDescription=std::move(*description);
        }

        auto candidates=link.transport->takeLocalCandidates();
        link.localCandidates.insert(
            link.localCandidates.end(),
            std::make_move_iterator(candidates.begin()),
            std::make_move_iterator(candidates.end())
        );

        if(link.localDescription && link.transport->gatheringComplete() && !link.signalSent) {
            const auto signal=encodeIceSignal({
                *link.localDescription,
                link.localCandidates
            });
            participant.signaling->sendSignal(remoteId,signal);
            ++participant.signalMessagesSent;
            ++gSignalMessagesSent;
            gMaxSignalMessagesPerSession=std::max(
                gMaxSignalMessagesPerSession,
                participant.signalMessagesSent);
            link.signalSent=true;
        }
    }
}

bool fullMeshConnected(const std::vector<std::unique_ptr<Participant>>& participants) {
    const auto expected=participants.size()-1;
    for(const auto& participant:participants) {
        if(!participant->roomReady || participant->links.size()!=expected) return false;
        for(const auto& [remoteId,link]:participant->links) {
            (void)remoteId;
            if(!link->transport->connected()) return false;
        }
    }
    return true;
}

std::size_t connectedEndpointCount(const std::vector<std::unique_ptr<Participant>>& participants) {
    std::size_t count=0;
    for(const auto& participant:participants) {
        for(const auto& [remoteId,link]:participant->links) {
            (void)remoteId;
            if(link->transport->connected()) ++count;
        }
    }
    return count;
}

void verifyOverflowRejected(
    const std::string& server,
    const std::string& roomCode,
    int participantIndex) {
    SignalingClient overflow(server);
    overflow.setRoomJoin(
        roomCode,
        Participant::makeMemberId(participantIndex),
        "LoadOverflow"+std::to_string(participantIndex));

    const auto deadline=Clock::now()+std::chrono::seconds(10);
    while(Clock::now()<deadline) {
        overflow.service();
        for(const auto& event:overflow.takeEvents()) {
            switch(event.type) {
                case SignalingEventType::Error:
                    if(event.payload=="Room is full") return;
                    throw std::runtime_error(
                        "Overflow participant received unexpected signaling error: "+event.payload);
                case SignalingEventType::RoomJoined:
                    throw std::runtime_error("Overflow participant unexpectedly joined the full room");
                case SignalingEventType::TransportError:
                    throw std::runtime_error(
                        "Overflow participant signaling transport error: "+event.payload);
                case SignalingEventType::Closed:
                    throw std::runtime_error(
                        "Overflow participant signaling closed before capacity rejection");
                default:
                    break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    throw std::runtime_error("Timed out waiting for 13th-participant capacity rejection");
}

void waitForFullMesh(
    std::vector<std::unique_ptr<Participant>>& participants,
    int timeoutSeconds,
    std::string_view label) {
    const auto expectedEndpoints=participants.size()*(participants.size()-1);
    const auto deadline=Clock::now()+std::chrono::seconds(timeoutSeconds);
    auto nextReport=Clock::now()+std::chrono::seconds(1);

    while(!fullMeshConnected(participants)) {
        for(auto& participant:participants) processSignaling(*participant);

        const auto now=Clock::now();
        if(now>=nextReport) {
            std::cout
                <<"  "<<label<<" ICE endpoints "
                <<connectedEndpointCount(participants)<<"/"<<expectedEndpoints<<"\n";
            nextReport=now+std::chrono::seconds(1);
        }
        if(now>=deadline) {
            throw std::runtime_error(
                std::string(label)+" timed out ("+
                std::to_string(connectedEndpointCount(participants))+"/"+
                std::to_string(expectedEndpoints)+" ICE endpoints connected)");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
}

std::unique_ptr<Participant> makeJoiningParticipant(
    int index,
    const LoadTestOptions& options,
    const std::string& roomCode,
    const OpusCodecSettings& settings) {
    auto participant=std::make_unique<Participant>(index);
    participant->encoder.configure(settings);
    participant->signaling=std::make_unique<SignalingClient>(options.server);
    participant->signaling->setRoomJoin(
        roomCode,
        participant->memberId,
        participant->name);
    return participant;
}

std::uint64_t verifyHardDropAndRejoin(
    std::vector<std::unique_ptr<Participant>>& participants,
    const LoadTestOptions& options,
    const std::string& roomCode,
    const OpusCodecSettings& settings,
    int participantIndex) {
    const auto signalsBefore=gSignalMessagesSent;
    const auto found=std::find_if(
        participants.begin(),
        participants.end(),
        [participantIndex](const auto& participant) {
            return participant->index==participantIndex;
        });
    if(found==participants.end()) {
        throw std::runtime_error("Hard-drop participant not found");
    }

    std::cout<<"Hard-dropping "<<(*found)->name<<"...\n";
    auto dropped=std::move(*found);
    participants.erase(found);
    dropped.reset();

    waitForFullMesh(
        participants,
        options.connectTimeoutSeconds,
        "Post-drop mesh");
    std::cout
        <<"Remaining "<<participants.size()
        <<" participants stayed fully connected\n";

    std::cout<<"Rejoining LoadPeer"<<participantIndex<<"...\n";
    participants.push_back(
        makeJoiningParticipant(
            participantIndex,
            options,
            roomCode,
            settings));
    waitForFullMesh(
        participants,
        options.connectTimeoutSeconds,
        "Rejoin mesh");
    const auto signalBurst=gSignalMessagesSent-signalsBefore;
    std::cout
        <<"Full mesh restored after hard drop/rejoin; signaling burst "
        <<signalBurst<<" messages\n";
    return signalBurst;
}

std::uint64_t verifyStaleSocketReplacement(
    std::vector<std::unique_ptr<Participant>>& participants,
    const LoadTestOptions& options,
    const std::string& roomCode,
    const OpusCodecSettings& settings,
    int participantIndex) {
    const auto signalsBefore=gSignalMessagesSent;
    const auto found=std::find_if(
        participants.begin(),
        participants.end(),
        [participantIndex](const auto& participant) {
            return participant->index==participantIndex;
        });
    if(found==participants.end()) {
        throw std::runtime_error("Stale-reconnect participant not found");
    }

    std::cout
        <<"Replacing live signaling socket for LoadPeer"
        <<participantIndex<<" with the same member ID...\n";

    auto stale=std::move(*found);
    participants.erase(found);
    participants.push_back(
        makeJoiningParticipant(
            participantIndex,
            options,
            roomCode,
            settings));

    waitForFullMesh(
        participants,
        options.connectTimeoutSeconds,
        "Stale-reconnect mesh");

    bool staleClosed=false;
    const auto closeDeadline=Clock::now()+std::chrono::seconds(10);
    while(Clock::now()<closeDeadline && !staleClosed) {
        stale->signaling->service();
        for(const auto& event:stale->signaling->takeEvents()) {
            if(event.type==SignalingEventType::Closed) {
                staleClosed=true;
                break;
            }
        }
        if(!staleClosed) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if(!staleClosed) {
        throw std::runtime_error(
            "Server did not close the stale signaling socket after same-ID reconnect");
    }

    stale.reset();
    const auto signalBurst=gSignalMessagesSent-signalsBefore;
    std::cout
        <<"Stale socket closed and full mesh restored for LoadPeer"
        <<participantIndex<<"; signaling burst "
        <<signalBurst<<" messages\n";
    return signalBurst;
}

std::uint64_t verifyRoomChangeAndReturn(
    std::vector<std::unique_ptr<Participant>>& participants,
    const LoadTestOptions& options,
    const std::string& originalRoomCode,
    int participantIndex) {
    const auto signalsBefore=gSignalMessagesSent;
    const auto found=std::find_if(
        participants.begin(),
        participants.end(),
        [participantIndex](const auto& participant) {
            return participant->index==participantIndex;
        });
    if(found==participants.end()) {
        throw std::runtime_error("Room-change participant not found");
    }

    auto moved=std::move(*found);
    participants.erase(found);

    std::cout
        <<"Moving "<<moved->name
        <<" into a temporary room...\n";
    moved->signaling->setRoomCreate(moved->memberId,moved->name);

    const auto moveDeadline=
        Clock::now()+std::chrono::seconds(options.connectTimeoutSeconds);
    while(
        moved->roomCode==originalRoomCode ||
        !fullMeshConnected(participants)) {
        processSignaling(*moved);
        for(auto& participant:participants) processSignaling(*participant);

        if(Clock::now()>=moveDeadline) {
            throw std::runtime_error(
                "Timed out moving participant away while preserving the remaining mesh");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }

    if(moved->roomCode.empty() || moved->roomCode==originalRoomCode) {
        throw std::runtime_error("Temporary room was not created");
    }
    if(!moved->links.empty()) {
        throw std::runtime_error(
            "Room-changing participant retained stale peer links");
    }

    const auto temporaryRoom=moved->roomCode;
    std::cout
        <<"Temporary room "<<temporaryRoom
        <<" active; remaining "<<participants.size()
        <<" participants stayed fully connected\n";

    moved->signaling->setRoomJoin(
        originalRoomCode,
        moved->memberId,
        moved->name);
    participants.push_back(std::move(moved));

    waitForFullMesh(
        participants,
        options.connectTimeoutSeconds,
        "Room-return mesh");

    const auto signalBurst=gSignalMessagesSent-signalsBefore;
    std::cout
        <<"Original 12-participant mesh restored after room change; signaling burst "
        <<signalBurst<<" messages\n";
    return signalBurst;
}

std::uint64_t verifyDualDropAndRejoin(
    std::vector<std::unique_ptr<Participant>>& participants,
    const LoadTestOptions& options,
    const std::string& roomCode,
    const OpusCodecSettings& settings,
    int firstIndex,
    int secondIndex) {
    const auto signalsBefore=gSignalMessagesSent;
    std::array<std::unique_ptr<Participant>,2> dropped;
    const std::array<int,2> indices{firstIndex,secondIndex};

    for(std::size_t i=0;i<indices.size();++i) {
        const auto found=std::find_if(
            participants.begin(),
            participants.end(),
            [index=indices[i]](const auto& participant) {
                return participant->index==index;
            });
        if(found==participants.end()) {
            throw std::runtime_error("Dual-drop participant not found");
        }
        dropped[i]=std::move(*found);
        participants.erase(found);
    }

    std::cout
        <<"Hard-dropping LoadPeer"<<firstIndex
        <<" and LoadPeer"<<secondIndex<<" simultaneously...\n";
    dropped[0].reset();
    dropped[1].reset();

    waitForFullMesh(
        participants,
        options.connectTimeoutSeconds,
        "Dual-drop remaining mesh");
    std::cout
        <<"Remaining "<<participants.size()
        <<" participants stayed fully connected\n";

    participants.push_back(
        makeJoiningParticipant(
            firstIndex,
            options,
            roomCode,
            settings));
    participants.push_back(
        makeJoiningParticipant(
            secondIndex,
            options,
            roomCode,
            settings));

    waitForFullMesh(
        participants,
        options.connectTimeoutSeconds,
        "Dual-rejoin mesh");

    const auto signalBurst=gSignalMessagesSent-signalsBefore;
    std::cout
        <<"Full mesh restored after dual reconnect; signaling burst "
        <<signalBurst<<" messages\n";
    return signalBurst;
}

void makeTone(Participant& participant,std::span<std::int16_t> samples) {
    constexpr double Pi=3.14159265358979323846;
    const double frequency=220.0+participant.index*27.0;
    const double step=2.0*Pi*frequency/static_cast<double>(VoiceFormat::SampleRate);

    for(auto& sample:samples) {
        sample=static_cast<std::int16_t>(std::sin(participant.phase)*2400.0);
        participant.phase+=step;
        if(participant.phase>=2.0*Pi) participant.phase-=2.0*Pi;
    }
}

void sendSyntheticFrame(Participant& participant) {
    std::array<std::int16_t,VoiceFormat::FrameSamples> samples{};
    std::array<std::byte,VoiceFormat::MaxVoicePacketBytes> packet{};

    makeTone(participant,samples);

    packet[0]=static_cast<std::byte>('M');
    packet[1]=static_cast<std::byte>('K');
    packet[2]=static_cast<std::byte>('W');
    packet[3]=static_cast<std::byte>('V');
    packet[4]=static_cast<std::byte>(PacketVersion);
    packet[5]=static_cast<std::byte>(0);
    writeStreamId(packet,participant.streamId);
    writeSequence(packet,participant.sequence++);

    const auto payload=participant.encoder.encode(
        samples,
        std::span<std::byte>(packet).subspan(VoiceFormat::PacketHeaderBytes)
    );
    const auto packetSize=VoiceFormat::PacketHeaderBytes+payload;

    for(auto& [remoteId,link]:participant.links) {
        (void)remoteId;
        if(!link->transport->connected()) continue;
        if(link->transport->send(std::span<const std::byte>(packet.data(),packetSize))) {
            ++participant.txPackets;
            participant.txBytes+=packetSize;
        }
    }
}

void receivePackets(PeerLink& link) {
    std::array<std::byte,VoiceFormat::MaxVoicePacketBytes> packet{};

    for(int drained=0;drained<64;++drained) {
        const auto size=link.transport->tryReceive(packet);
        if(size==0) break;

        const auto received=std::span<const std::byte>(packet.data(),size);
        if(!validVoicePacket(received)) continue;

        ++link.rxPackets;
        link.rxBytes+=size;

        const auto streamId=readStreamId(received);
        if(!link.haveStream || link.streamId!=streamId) {
            link.haveStream=true;
            link.streamId=streamId;
            link.decoder.reset();
            link.jitter.reset(false);
        }

        const auto sequence=readSequence(received);
        auto result=link.jitter.push(
            sequence,
            received.subspan(VoiceFormat::PacketHeaderBytes)
        );

        if(result==JitterBufferPushResult::TooFarAhead) {
            link.decoder.reset();
            link.jitter.reset(true);
            link.jitter.push(sequence,received.subspan(VoiceFormat::PacketHeaderBytes));
        }
    }
}

bool decodeOne(PeerLink& link,std::span<std::int16_t> output) {
    const auto ready=link.jitter.popReady();
    if(!ready) return false;

    try {
        std::size_t decoded=0;
        if(ready->kind==JitterBufferFrameKind::Packet) {
            decoded=link.decoder.decode(
                std::span<const std::byte>(ready->payload.data(),ready->payloadSize),
                output,
                false
            );
        } else if(ready->kind==JitterBufferFrameKind::Missing && ready->fecPayloadSize>0) {
            decoded=link.decoder.decode(
                std::span<const std::byte>(ready->fecPayload.data(),ready->fecPayloadSize),
                output,
                true
            );
        } else {
            decoded=link.decoder.conceal(output);
        }

        if(decoded<output.size()) {
            std::fill(output.begin()+static_cast<std::ptrdiff_t>(decoded),output.end(),0);
        }
        ++link.decodedFrames;
        return true;
    } catch(...) {
        ++link.decoderErrors;
        return false;
    }
}

void receiveAndMix(Participant& participant) {
    std::array<std::int32_t,VoiceFormat::FrameSamples> mix{};
    std::array<std::int16_t,VoiceFormat::FrameSamples> decoded{};
    bool any=false;

    for(auto& [remoteId,link]:participant.links) {
        (void)remoteId;
        receivePackets(*link);
        if(!decodeOne(*link,decoded)) continue;

        any=true;
        for(std::size_t i=0;i<decoded.size();++i) {
            mix[i]+=decoded[i];
        }
    }

    if(!any) return;
    ++participant.mixedFrames;
    for(const auto sample:mix) {
        if(sample>32767 || sample<-32768) ++participant.clippedSamples;
    }
}

void printUsage() {
    std::cout
        <<"MKW VoiceChat headless load test\n"
        <<"Usage:\n"
        <<"  mkw_voicechat --loadtest --peers 4\n"
        <<"  mkw_voicechat --loadtest --peers 6 --duration 20\n"
        <<"  mkw_voicechat --loadtest --peers 12 --duration 30\n"
        <<"Options:\n"
        <<"  --peers N             Synthetic participants, 2-12 (default 12)\n"
        <<"  --duration N          Voice traffic seconds, 5-300 (default 15)\n"
        <<"  --connect-timeout N   Full-mesh setup timeout, 10-180s (default 60)\n"
        <<"  --server URL          Signaling WebSocket endpoint\n";
}

}

bool loadTestRequested(std::span<const std::string> args) {
    return std::find(args.begin(),args.end(),"--loadtest")!=args.end();
}

int runLoadTest(std::span<const std::string> args) {
    try {
        gSignalMessagesSent=0;
        gMaxSignalMessagesPerSession=0;
        if(std::find(args.begin(),args.end(),"--help")!=args.end() ||
           std::find(args.begin(),args.end(),"-h")!=args.end()) {
            printUsage();
            return 0;
        }

        const auto options=parseOptions(args);
        const auto expectedEndpoints=
            static_cast<std::size_t>(options.peers)*static_cast<std::size_t>(options.peers-1);
        const auto expectedPairs=expectedEndpoints/2;

        std::cout
            <<"MKW VoiceChat headless load test\n"
            <<"Participants: "<<options.peers<<"\n"
            <<"Expected P2P pairs: "<<expectedPairs<<"\n"
            <<"Expected ICE endpoints: "<<expectedEndpoints<<"\n"
            <<"Traffic duration: "<<options.durationSeconds<<"s\n"
            <<"Signaling: "<<options.server<<"\n\n";

        std::vector<std::unique_ptr<Participant>> participants;
        participants.reserve(static_cast<std::size_t>(options.peers));

        auto creator=std::make_unique<Participant>(1);
        creator->signaling=std::make_unique<SignalingClient>(options.server);
        creator->signaling->setRoomCreate(creator->memberId,creator->name);
        participants.push_back(std::move(creator));

        const auto roomDeadline=Clock::now()+std::chrono::seconds(15);
        while(participants.front()->roomCode.empty()) {
            processSignaling(*participants.front());
            if(Clock::now()>=roomDeadline) throw std::runtime_error("Timed out creating load-test room");
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        const std::string roomCode=participants.front()->roomCode;
        std::cout<<"Room: "<<roomCode<<"\n";

        for(int i=2;i<=options.peers;++i) {
            auto participant=std::make_unique<Participant>(i);
            participant->signaling=std::make_unique<SignalingClient>(options.server);
            participant->signaling->setRoomJoin(roomCode,participant->memberId,participant->name);
            participants.push_back(std::move(participant));
        }

        std::cout<<"Building full mesh...\n";
        const auto connectStart=Clock::now();
        const auto connectDeadline=connectStart+std::chrono::seconds(options.connectTimeoutSeconds);
        auto lastProgress=connectStart;

        while(!fullMeshConnected(participants)) {
            for(auto& participant:participants) processSignaling(*participant);

            const auto now=Clock::now();
            if(now-lastProgress>=std::chrono::seconds(1)) {
                const auto connected=connectedEndpointCount(participants);
                std::cout<<"  ICE endpoints "<<connected<<"/"<<expectedEndpoints<<"\n";
                lastProgress=now;
            }

            if(now>=connectDeadline) {
                throw std::runtime_error(
                    "Timed out building full mesh ("+
                    std::to_string(connectedEndpointCount(participants))+"/"+
                    std::to_string(expectedEndpoints)+" ICE endpoints connected)"
                );
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }

        const auto connectElapsed=std::chrono::duration<double>(Clock::now()-connectStart).count();
        const auto initialMeshSignalMessages=gSignalMessagesSent;
        std::cout
            <<"Full mesh connected: "<<expectedPairs<<" pairs / "<<expectedEndpoints
            <<" endpoints in "<<std::fixed<<std::setprecision(2)<<connectElapsed<<"s\n"
            <<"Initial mesh signaling messages: "<<initialMeshSignalMessages<<"\n"
            <<"Maximum signals from one session so far: "
            <<gMaxSignalMessagesPerSession<<"\n";

        if(options.peers==12) {
            std::cout<<"Checking 13th-participant capacity rejection...\n";
            verifyOverflowRejected(options.server,roomCode,13);
            std::cout<<"13th participant rejected cleanly; existing 12 retained\n";
        }

        OpusCodecSettings settings;
        settings.bitrate=48000;
        settings.complexity=10;
        settings.expectedPacketLossPercent=5;
        settings.inbandFec=true;
        settings.dtx=false;
        for(auto& participant:participants) participant->encoder.configure(settings);

        if(options.peers==12) {
            std::cout<<"\n=== RECONNECT / CHURN GATE ===\n";
            const auto hardDropBurst1=verifyHardDropAndRejoin(
                participants,
                options,
                roomCode,
                settings,
                12);
            const auto staleReplaceBurst=verifyStaleSocketReplacement(
                participants,
                options,
                roomCode,
                settings,
                11);
            const auto hardDropBurst2=verifyHardDropAndRejoin(
                participants,
                options,
                roomCode,
                settings,
                10);
            const auto roomChangeBurst=verifyRoomChangeAndReturn(
                participants,
                options,
                roomCode,
                9);
            const auto dualReconnectBurst=verifyDualDropAndRejoin(
                participants,
                options,
                roomCode,
                settings,
                8,
                7);
            std::cout
                <<"Reconnect / churn gate passed\n"
                <<"Initial mesh signals: "<<initialMeshSignalMessages<<"\n"
                <<"Hard drop/rejoin burst #1: "<<hardDropBurst1<<"\n"
                <<"Stale replacement burst: "<<staleReplaceBurst<<"\n"
                <<"Hard drop/rejoin burst #2: "<<hardDropBurst2<<"\n"
                <<"Room change/return burst: "<<roomChangeBurst<<"\n"
                <<"Dual reconnect burst: "<<dualReconnectBurst<<"\n"
                <<"Total signaling messages through churn gate: "
                <<gSignalMessagesSent<<"\n"
                <<"Maximum signals sent by one signaling session: "
                <<gMaxSignalMessagesPerSession<<"\n\n";
        }

        const auto processBefore=processSnapshot();
        const auto trafficStart=Clock::now();
        const auto trafficEnd=trafficStart+std::chrono::seconds(options.durationSeconds);
        auto nextFrame=trafficStart;
        auto nextReport=trafficStart+std::chrono::seconds(1);

        while(Clock::now()<trafficEnd) {
            for(auto& participant:participants) {
                participant->signaling->service();
                processSignaling(*participant);
            }

            const auto now=Clock::now();
            if(now>=nextFrame) {
                for(auto& participant:participants) sendSyntheticFrame(*participant);
                for(auto& participant:participants) receiveAndMix(*participant);
                nextFrame+=std::chrono::milliseconds(VoiceFormat::FrameDurationMs);
                if(nextFrame+std::chrono::milliseconds(100)<now) nextFrame=now;
            } else {
                for(auto& participant:participants) receiveAndMix(*participant);
            }

            if(now>=nextReport) {
                std::uint64_t tx=0;
                std::uint64_t rx=0;
                for(const auto& participant:participants) {
                    tx+=participant->txPackets;
                    for(const auto& [remoteId,link]:participant->links) {
                        (void)remoteId;
                        rx+=link->rxPackets;
                    }
                }
                std::cout<<"  traffic TX "<<tx<<" packets, RX "<<rx<<" packets\n";
                nextReport+=std::chrono::seconds(1);
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        for(int i=0;i<20;++i) {
            for(auto& participant:participants) receiveAndMix(*participant);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        const auto trafficElapsed=std::chrono::duration<double>(Clock::now()-trafficStart).count();
        const auto processAfter=processSnapshot();

        std::uint64_t txPackets=0;
        std::uint64_t txBytes=0;
        std::uint64_t rxPackets=0;
        std::uint64_t rxBytes=0;
        std::uint64_t decodedFrames=0;
        std::uint64_t decoderErrors=0;
        std::uint64_t mixedFrames=0;
        std::uint64_t clippedSamples=0;
        std::uint64_t linksWithoutPackets=0;
        std::uint64_t linksWithoutDecodedFrames=0;
        std::uint64_t minLinkPackets=std::numeric_limits<std::uint64_t>::max();
        std::uint64_t minLinkDecodedFrames=std::numeric_limits<std::uint64_t>::max();

        for(const auto& participant:participants) {
            txPackets+=participant->txPackets;
            txBytes+=participant->txBytes;
            mixedFrames+=participant->mixedFrames;
            clippedSamples+=participant->clippedSamples;
            for(const auto& [remoteId,link]:participant->links) {
                (void)remoteId;
                rxPackets+=link->rxPackets;
                rxBytes+=link->rxBytes;
                decodedFrames+=link->decodedFrames;
                decoderErrors+=link->decoderErrors;
                minLinkPackets=std::min(minLinkPackets,link->rxPackets);
                minLinkDecodedFrames=std::min(minLinkDecodedFrames,link->decodedFrames);
                if(link->rxPackets==0) ++linksWithoutPackets;
                if(link->decodedFrames==0) ++linksWithoutDecodedFrames;
            }
        }

        const double delivery=txPackets==0 ? 0.0 : static_cast<double>(rxPackets)*100.0/static_cast<double>(txPackets);
        const double uploadMbit=trafficElapsed<=0.0 ? 0.0 : static_cast<double>(txBytes)*8.0/trafficElapsed/1000000.0;
        const double downloadMbit=trafficElapsed<=0.0 ? 0.0 : static_cast<double>(rxBytes)*8.0/trafficElapsed/1000000.0;
        const double cpuSeconds=std::max(0.0,processAfter.cpuSeconds-processBefore.cpuSeconds);
        const double coreCpu=trafficElapsed<=0.0 ? 0.0 : cpuSeconds/trafficElapsed*100.0;
        const auto hardwareThreads=std::max(1u,std::thread::hardware_concurrency());
        const double machineCpu=coreCpu/static_cast<double>(hardwareThreads);
        const double residentMb=static_cast<double>(processAfter.residentBytes)/(1024.0*1024.0);

        if(minLinkPackets==std::numeric_limits<std::uint64_t>::max()) minLinkPackets=0;
        if(minLinkDecodedFrames==std::numeric_limits<std::uint64_t>::max()) minLinkDecodedFrames=0;

        const bool meshStillConnected=fullMeshConnected(participants);
        const bool packetFlowOk=
            txPackets>0 &&
            rxPackets>0 &&
            delivery>=95.0 &&
            linksWithoutPackets==0;
        const bool decodeOk=
            decodedFrames>0 &&
            decoderErrors==0 &&
            linksWithoutDecodedFrames==0;
        const bool pass=meshStillConnected && packetFlowOk && decodeOk;

        std::cout
            <<"\n=== LOAD TEST RESULT ===\n"
            <<"Result: "<<(pass ? "PASS" : "FAIL")<<"\n"
            <<"Participants: "<<options.peers<<"\n"
            <<"P2P pairs: "<<expectedPairs<<"/"<<expectedPairs<<"\n"
            <<"ICE endpoints: "<<connectedEndpointCount(participants)<<"/"<<expectedEndpoints
            <<(meshStillConnected ? " (stable)" : " (connection lost)")<<"\n"
            <<"TX packets: "<<txPackets<<"\n"
            <<"RX packets: "<<rxPackets<<"\n"
            <<"Delivery: "<<std::fixed<<std::setprecision(2)<<delivery<<"%\n"
            <<"Decoded frames: "<<decodedFrames<<"\n"
            <<"Decoder errors: "<<decoderErrors<<"\n"
            <<"Links without packets: "<<linksWithoutPackets<<"\n"
            <<"Links without decoded frames: "<<linksWithoutDecodedFrames<<"\n"
            <<"Minimum packets on one link: "<<minLinkPackets<<"\n"
            <<"Minimum decoded frames on one link: "<<minLinkDecodedFrames<<"\n"
            <<"Average upload per participant: "<<(uploadMbit/static_cast<double>(options.peers))<<" Mbit/s\n"
            <<"Average download per participant: "<<(downloadMbit/static_cast<double>(options.peers))<<" Mbit/s\n"
            <<"Mixed participant frames: "<<mixedFrames<<"\n"
            <<"Mixer clipped samples: "<<clippedSamples<<"\n"
            <<"Aggregate upload: "<<std::fixed<<std::setprecision(2)<<uploadMbit<<" Mbit/s\n"
            <<"Aggregate download: "<<downloadMbit<<" Mbit/s\n"
            <<"Process CPU: "<<coreCpu<<"% of one core-equivalent\n"
            <<"Process CPU normalized: "<<machineCpu<<"% of "<<hardwareThreads<<" logical CPUs\n"
            <<"Peak resident memory: "<<residentMb<<" MiB\n";

        for(auto& participant:participants) {
            try {
                participant->signaling->setRoomNone();
            } catch(...) {}
        }

        return pass ? 0 : 2;
    } catch(const std::exception& error) {
        std::cerr<<"LOAD TEST ERROR: "<<error.what()<<"\n";
        std::cerr<<"Use --loadtest --help for options.\n";
        return 1;
    }
}

}

#else

#include <algorithm>
#include <iostream>

namespace mkwvc {

bool loadTestRequested(std::span<const std::string> args) {
    return std::find(args.begin(),args.end(),"--loadtest")!=args.end();
}

int runLoadTest(std::span<const std::string>) {
    std::cerr<<"This build has no ICE support; load testing is unavailable.\n";
    return 1;
}

}

#endif
