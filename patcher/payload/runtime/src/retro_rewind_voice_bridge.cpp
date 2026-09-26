#include "retro_rewind_voice_bridge.h"

#include "runtime_product.h"
#include "memory.h"
#include "mkwvc/EmbeddedCore.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <winhttp.h>
#endif

namespace RetroRewindVoiceBridge {
namespace {

constexpr std::uint16_t kGpcmPort = 29900;
constexpr std::size_t kMaxBufferedBytes = 32 * 1024;
constexpr std::string_view kFinal = "\\final\\";

struct SocketState {
    std::string inbound;
    std::string outbound;
    std::string gameName;
};

std::mutex g_mutex;
std::unordered_map<std::uint32_t, SocketState> g_sockets;
IdentitySnapshot g_identity;
std::uint32_t g_identitySocket = UINT32_MAX;

constexpr std::size_t kMaxRoomReplyBytes = 64 * 1024;

struct RoomLookupState {
    std::mutex mutex;
    std::condition_variable wake;
    RoomSnapshot snapshot;
    std::uint64_t observedIdentityGeneration = UINT64_MAX;
    std::uint64_t desiredIdentityGeneration = 0;
    std::uint64_t workerIdentityGeneration = UINT64_MAX;
    std::string desiredProfileId;
    bool desiredOnline = false;
    bool desiredRoomActive = false;
    bool observedRoomActive = false;
    bool workerRunning = false;
    std::chrono::steady_clock::time_point nextReconnectAttempt{};
};

RoomLookupState& RoomState() {
    // Intentionally process-lifetime storage: lookup workers are detached so
    // shutdown can never race a C++ static destructor for this state.
    static RoomLookupState* state = new RoomLookupState();
    return *state;
}

constexpr auto kReleaseCheckInterval = std::chrono::minutes(10);

struct ReleaseCheckState {
    std::mutex mutex;
    ReleaseStatus snapshot;
    bool workerRunning = false;
    std::chrono::steady_clock::time_point nextCheck{};
};

ReleaseCheckState& ReleaseState() {
    // Detached background check; retain storage for process lifetime.
    static ReleaseCheckState* state = new ReleaseCheckState();
    return *state;
}

bool ParseVersionTriplet(
    std::string_view text,
    std::array<int,3>& result) {
    result={0,0,0};
    std::size_t start=0;
    for(std::size_t index=0;index<3;++index) {
        const std::size_t end=
            index==2 ? text.size() : text.find('.',start);
        if(end==std::string_view::npos || end<=start) return false;
        const auto part=text.substr(start,end-start);
        int value=0;
        const auto parsed=std::from_chars(
            part.data(),
            part.data()+part.size(),
            value);
        if(parsed.ec!=std::errc{} ||
           parsed.ptr!=part.data()+part.size() ||
           value<0) {
            return false;
        }
        result[index]=value;
        start=end+1;
    }
    return start==text.size()+1;
}

int CompareVersions(
    std::string_view left,
    std::string_view right) {
    std::array<int,3> a{};
    std::array<int,3> b{};
    if(!ParseVersionTriplet(left,a) ||
       !ParseVersionTriplet(right,b)) {
        return 0;
    }
    if(a<b) return -1;
    if(a>b) return 1;
    return 0;
}

std::string JsonStringValue(
    std::string_view json,
    std::string_view key) {
    const std::string marker="\""+std::string(key)+"\"";
    const std::size_t keyPos=json.find(marker);
    if(keyPos==std::string_view::npos) return {};
    const std::size_t colon=json.find(':',keyPos+marker.size());
    if(colon==std::string_view::npos) return {};
    const std::size_t quote=json.find('"',colon+1);
    if(quote==std::string_view::npos) return {};
    const std::size_t end=json.find('"',quote+1);
    if(end==std::string_view::npos) return {};
    return std::string(json.substr(quote+1,end-quote-1));
}

bool JsonIntValue(
    std::string_view json,
    std::string_view key,
    int& value) {
    const std::string marker="\""+std::string(key)+"\"";
    const std::size_t keyPos=json.find(marker);
    if(keyPos==std::string_view::npos) return false;
    const std::size_t colon=json.find(':',keyPos+marker.size());
    if(colon==std::string_view::npos) return false;
    std::size_t start=colon+1;
    while(start<json.size() &&
          std::isspace(static_cast<unsigned char>(json[start]))!=0) {
        ++start;
    }
    std::size_t end=start;
    while(end<json.size() &&
          std::isdigit(static_cast<unsigned char>(json[end]))!=0) {
        ++end;
    }
    if(end==start) return false;
    const auto parsed=std::from_chars(
        json.data()+start,
        json.data()+end,
        value);
    return parsed.ec==std::errc{} &&
           parsed.ptr==json.data()+end;
}

bool IsDecimal(std::string_view value, bool allowNegative = false) {
    if (value.empty()) {
        return false;
    }
    std::size_t index = 0;
    if (allowNegative && value.front() == '-') {
        index = 1;
        if (index == value.size()) {
            return false;
        }
    }
    return std::all_of(value.begin() + static_cast<std::ptrdiff_t>(index), value.end(),
                       [](unsigned char ch) { return std::isdigit(ch) != 0; });
}

std::string ValueForKey(std::string_view message, std::string_view key) {
    std::string marker;
    marker.reserve(key.size() + 2);
    marker.push_back('\\');
    marker.append(key);
    marker.push_back('\\');

    const std::size_t start = message.find(marker);
    if (start == std::string_view::npos) {
        return {};
    }
    const std::size_t valueStart = start + marker.size();
    const std::size_t valueEnd = message.find('\\', valueStart);
    if (valueEnd == std::string_view::npos) {
        return {};
    }
    return std::string(message.substr(valueStart, valueEnd - valueStart));
}

void TrimBuffer(std::string& buffer) {
    if (buffer.size() <= kMaxBufferedBytes) {
        return;
    }

    // Keep only a bounded tail. GameSpy commands are tiny compared with this
    // limit; retaining unbounded peer data would let one connection grow host
    // memory indefinitely.
    buffer.erase(0, buffer.size() - kMaxBufferedBytes);
}

void ClearIdentityLocked() {
    if (g_identity.online || !g_identity.profileId.empty() ||
        !g_identity.sessionKey.empty() || !g_identity.gameName.empty()) {
        ++g_identity.generation;
    }
    g_identity.online = false;
    g_identity.profileId.clear();
    g_identity.sessionKey.clear();
    g_identity.gameName.clear();
    g_identitySocket = UINT32_MAX;
}

void HandleOutboundLocked(std::uint32_t wiiFd, std::string_view message) {
    SocketState& state = g_sockets[wiiFd];

    if (const std::string gameName = ValueForKey(message, "gamename");
        !gameName.empty() && gameName.size() <= 32) {
        state.gameName = gameName;
    }

    // A GameSpy logout explicitly ends the live bearer session even before the
    // TCP socket is physically closed.
    if (message.find("\\logout\\") != std::string_view::npos &&
        g_identitySocket == wiiFd) {
        ClearIdentityLocked();
    }
}

void HandleInboundLocked(std::uint32_t wiiFd, std::string_view message) {
    // Successful GPCM login response ending with the GameSpy final terminator.
    // Example fields include lc=2, sesskey, profileid and the GameSpy final terminator.
    if (message.find("\\lc\\2\\") == std::string_view::npos) {
        return;
    }

    const std::string profileId = ValueForKey(message, "profileid");
    const std::string sessionKey = ValueForKey(message, "sesskey");
    if (!IsDecimal(profileId) || profileId.size() > 10 ||
        !IsDecimal(sessionKey, true) || sessionKey.size() > 11) {
        return;
    }

    SocketState& state = g_sockets[wiiFd];
    const std::string gameName = state.gameName.empty() ? "mariokartwii" : state.gameName;

    const bool changed = !g_identity.online ||
                         g_identity.profileId != profileId ||
                         g_identity.sessionKey != sessionKey ||
                         g_identity.gameName != gameName ||
                         g_identitySocket != wiiFd;

    g_identity.online = true;
    g_identity.profileId = profileId;
    g_identity.sessionKey = sessionKey;
    g_identity.gameName = gameName;
    g_identitySocket = wiiFd;
    if (changed) {
        ++g_identity.generation;
    }
}

template <typename Handler>
void FeedLocked(std::uint32_t wiiFd, std::string& buffer,
                const std::uint8_t* data, std::size_t size, Handler&& handler) {
    if (!data || size == 0) {
        return;
    }

    buffer.append(reinterpret_cast<const char*>(data), size);
    TrimBuffer(buffer);

    while (true) {
        const std::size_t end = buffer.find(kFinal);
        if (end == std::string::npos) {
            break;
        }

        const std::size_t packetEnd = end + kFinal.size();
        std::string packet = buffer.substr(0, packetEnd);
        buffer.erase(0, packetEnd);
        handler(wiiFd, packet);
    }
}

bool ShouldObserve(std::uint16_t peerPort) {
    // Some GameSpy sockets are nonblocking and can exchange their first packet
    // before the HLE has recorded the connected peer port. Accept port 0 as an
    // unknown-yet candidate; the parser still requires the exact GPCM login
    // message shape before it captures anything.
    return RuntimeProduct::IsRetroRewind() &&
           (peerPort == 0 || peerPort == kGpcmPort);
}

// Mario Kart Wii RKNet::Controller layout. These offsets are from the PAL
// Controller layout used by Retro Rewind's own GameSource:
//   sInstance                 0x809C20D8
//   matchmaking infos         +0x38, 0x58 bytes each
//   roomType                  +0xE8
//   current matchmaking info  +0x291C
// MatchMakingInfo:
//   connected consoles        +0x08
//   full AID bitmap           +0x10
//   local AID                 +0x21
constexpr std::uint32_t kRkNetControllerInstance = 0x809C20D8u;
constexpr std::uint32_t kRkNetFriendMgrInstance = 0x809C2110u;
constexpr std::uint32_t kFriendMgrFriendPids = 0x36Cu;
constexpr std::uint32_t kFriendMgrFriendCount = 30u;
constexpr std::uint32_t kControllerMatchInfo = 0x38u;
constexpr std::uint32_t kMatchInfoSize = 0x58u;
constexpr std::uint32_t kControllerRoomType = 0xE8u;
constexpr std::uint32_t kControllerCurrentMatchInfo = 0x291Cu;
constexpr std::uint32_t kControllerSplitToSend = 0xF0u;
constexpr std::uint32_t kControllerSplitReceived = 0x150u;
constexpr std::uint32_t kControllerLastSendBufferUsed = 0x276Cu;
constexpr std::uint32_t kControllerLastReceivedBufferUsed = 0x279Cu;
constexpr std::uint32_t kControllerPlayerAidMap = 0x2920u;
constexpr std::uint32_t kMatchConnectedConsoles = 0x08u;
constexpr std::uint32_t kMatchFullAidBitmap = 0x10u;
constexpr std::uint32_t kMatchGroupId = 0x18u;
constexpr std::uint32_t kMatchLocalAid = 0x21u;
constexpr std::uint32_t kMatchHostAid = 0x22u;
constexpr std::uint32_t kRoomTypeNone = 0u;
constexpr std::uint32_t kRoomSectionIndex = 3u;
constexpr std::uint32_t kPacketHolderPacket = 0x0u;
constexpr std::uint32_t kPacketHolderSize = 0x8u;
constexpr std::uint32_t kPulRoomHostSystemContext = 0x4u;
constexpr std::uint32_t kPulRoomExtendedTeams = 0x15u;
constexpr std::uint32_t kPulRoomTeamBytes = 6u;
constexpr std::uint32_t kPulRoomMinimumTeamSize = kPulRoomExtendedTeams + kPulRoomTeamBytes;
constexpr std::uint8_t kExtendedTeamUpdateMessage = 0x82u;
constexpr std::uint32_t kExtendedTeamsContextBit = 20u;
constexpr std::uint32_t kRacedataInstance = 0x809BD728u;
constexpr std::uint32_t kRacedataRacesScenario = 0x20u;
constexpr std::uint32_t kRacedataMenusScenario = 0xC10u;
constexpr std::uint32_t kScenarioPlayerCount = 0x4u;
constexpr std::uint32_t kScenarioPlayers = 0x8u;
constexpr std::uint32_t kScenarioSettings = 0xB48u;
constexpr std::uint32_t kSettingsModeFlags = 0x28u;
constexpr std::uint32_t kPlayerSize = 0xF0u;
constexpr std::uint32_t kPlayerTeam = 0xCCu;
constexpr std::uint32_t kTeamsEnabledFlag = 0x2u;
constexpr std::uint32_t kDwcMatchControlInstance = 0x8038630Cu;
constexpr std::uint32_t kDwcMatchNodes = 0x38u;
constexpr std::uint32_t kDwcNodeSize = 0x30u;
constexpr std::uint32_t kDwcNodePid = 0x0u;
constexpr std::uint32_t kDwcNodeAid = 0x16u;
constexpr std::uint32_t kDwcNodeCount = 32u;

bool IsLocalRkNetRoomActive() noexcept {
    try {
        if (!RuntimeProduct::IsRetroRewind() ||
            !Memory::Contains(kRkNetControllerInstance, 4)) {
            return false;
        }

        const std::uint32_t controller = Memory::Read32(kRkNetControllerInstance);
        if (controller == 0 ||
            !Memory::Contains(controller, kControllerCurrentMatchInfo + 4)) {
            return false;
        }

        const std::uint32_t roomType =
            Memory::Read32(controller + kControllerRoomType);
        if (roomType == kRoomTypeNone) {
            return false;
        }

        const std::uint32_t current =
            Memory::Read32(controller + kControllerCurrentMatchInfo);
        if (current > 1) {
            return false;
        }

        const std::uint32_t match =
            controller + kControllerMatchInfo + current * kMatchInfoSize;
        if (!Memory::Contains(match, kMatchInfoSize)) {
            return false;
        }

        const std::uint32_t connected =
            Memory::Read32(match + kMatchConnectedConsoles);
        const std::uint32_t fullAidBitmap =
            Memory::Read32(match + kMatchFullAidBitmap);
        const std::uint8_t localAid =
            Memory::Read8(match + kMatchLocalAid);

        // Mirrors the game's "has found match" condition: our own AID must be
        // present and at least one other console must actually be connected.
        return localAid < 12 &&
               connected > 1 &&
               (fullAidBitmap & (1u << localAid)) != 0;
    } catch (...) {
        return false;
    }
}


struct RkNetRoomState {
    std::uint32_t controller = 0;
    std::uint32_t match = 0;
    std::uint32_t availableAids = 0;
    std::uint32_t groupId = 0;
    std::uint8_t localAid = 0xFF;
    std::uint8_t hostAid = 0xFF;
};

struct TeamMembershipSnapshot {
    bool active = false;
    std::vector<std::string> knownProfileIds;
    std::vector<std::string> teammateProfileIds;
};

struct TeamRuntimeCache {
    std::uint64_t identityGeneration = UINT64_MAX;
    std::uint32_t groupId = 0;
    bool extendedActive = false;
    std::array<std::uint8_t, 12> extendedTeams{};

    TeamRuntimeCache() {
        extendedTeams.fill(0xFF);
    }
};

TeamRuntimeCache& TeamCache() {
    static TeamRuntimeCache cache;
    return cache;
}

bool ReadRkNetRoomState(RkNetRoomState& result) noexcept {
    try {
        if (!Memory::Contains(kRkNetControllerInstance, 4)) return false;
        result.controller = Memory::Read32(kRkNetControllerInstance);
        if (result.controller == 0 ||
            !Memory::Contains(result.controller, kControllerPlayerAidMap + 12)) return false;

        const std::uint32_t current =
            Memory::Read32(result.controller + kControllerCurrentMatchInfo);
        if (current > 1) return false;

        result.match =
            result.controller + kControllerMatchInfo + current * kMatchInfoSize;
        if (!Memory::Contains(result.match, kMatchInfoSize)) return false;

        result.availableAids=Memory::Read32(result.match+kMatchFullAidBitmap);
        result.groupId=Memory::Read32(result.match+kMatchGroupId);
        result.localAid=Memory::Read8(result.match+kMatchLocalAid);
        result.hostAid=Memory::Read8(result.match+kMatchHostAid);
        return result.localAid < 12 && result.hostAid < 12;
    } catch (...) {
        return false;
    }
}

std::vector<std::string> ReadRoomProfileIds(
    const std::string& localProfileId,
    bool localRoomActive) noexcept {
    std::vector<std::string> result;
    if(!localRoomActive) return result;

    try {
        RkNetRoomState room;
        if(!ReadRkNetRoomState(room)) return result;

        std::unordered_set<std::string> profiles;
        if(!localProfileId.empty()) profiles.insert(localProfileId);

        if(Memory::Contains(kDwcMatchControlInstance,4)) {
            const std::uint32_t matchControl=
                Memory::Read32(kDwcMatchControlInstance);
            if(matchControl!=0 &&
               Memory::Contains(
                   matchControl+kDwcMatchNodes,
                   kDwcNodeSize*kDwcNodeCount)) {
                for(std::uint32_t i=0;i<kDwcNodeCount;++i) {
                    const std::uint32_t node=
                        matchControl+kDwcMatchNodes+i*kDwcNodeSize;
                    const std::uint32_t pid=
                        Memory::Read32(node+kDwcNodePid);
                    const std::uint8_t aid=
                        Memory::Read8(node+kDwcNodeAid);
                    if(pid==0 ||
                       aid>=12 ||
                       (room.availableAids&(1u<<aid))==0) {
                        continue;
                    }
                    profiles.insert(std::to_string(pid));
                }
            }
        }

        result.assign(profiles.begin(),profiles.end());
        std::sort(result.begin(),result.end());
    } catch(...) {
        result.clear();
        if(!localProfileId.empty()) result.push_back(localProfileId);
    }

    return result;
}

bool ReadRoomSectionPacket(const RkNetRoomState& room,
                           std::uint8_t aid,
                           bool received,
                           std::uint32_t& packet,
                           std::uint32_t& size) noexcept {
    try {
        if (aid >= 12) return false;

        std::uint32_t buffer = 0;
        std::uint32_t splitSlot = 0;
        if (received) {
            const std::uint32_t index=aid*8u+kRoomSectionIndex;
            buffer=Memory::Read32(
                room.controller+kControllerLastReceivedBufferUsed+index*4u);
            if (buffer > 1) return false;
            splitSlot=
                room.controller+kControllerSplitReceived+
                (buffer*12u+aid)*4u;
        } else {
            buffer=Memory::Read32(
                room.controller+kControllerLastSendBufferUsed+aid*4u);
            if (buffer > 1) return false;
            splitSlot=
                room.controller+kControllerSplitToSend+
                (buffer*12u+aid)*4u;
        }

        if (!Memory::Contains(splitSlot,4)) return false;
        const std::uint32_t split=Memory::Read32(splitSlot);
        if (split==0 ||
            !Memory::Contains(split+kRoomSectionIndex*4u,4)) return false;

        const std::uint32_t holder=
            Memory::Read32(split+kRoomSectionIndex*4u);
        if (holder==0 || !Memory::Contains(holder,12)) return false;

        packet=Memory::Read32(holder+kPacketHolderPacket);
        size=Memory::Read32(holder+kPacketHolderSize);
        return packet!=0 &&
               size>=4 &&
               size<=0x200 &&
               Memory::Contains(packet,size);
    } catch (...) {
        return false;
    }
}

bool ReadCurrentTeamRoomPacket(const RkNetRoomState& room,
                               std::uint32_t& packet,
                               std::uint32_t& size) noexcept {
    if (room.localAid != room.hostAid) {
        return ReadRoomSectionPacket(room,room.hostAid,true,packet,size);
    }

    for (std::uint8_t aid=0;aid<12;++aid) {
        if (aid==room.localAid ||
            (room.availableAids&(1u<<aid))==0) continue;
        if (ReadRoomSectionPacket(room,aid,false,packet,size)) return true;
    }
    return false;
}

void ResetTeamCache(TeamRuntimeCache& cache,
                    std::uint64_t identityGeneration,
                    std::uint32_t groupId) {
    cache.identityGeneration=identityGeneration;
    cache.groupId=groupId;
    cache.extendedActive=false;
    cache.extendedTeams.fill(0xFF);
}

void ReadExtendedTeamsFromPacket(
    std::uint32_t packet,
    std::array<std::uint8_t,12>& teams) {
    for (std::uint32_t i=0;i<12;++i) {
        const std::uint8_t packed=
            Memory::Read8(packet+kPulRoomExtendedTeams+i/2u);
        const std::uint8_t team=
            static_cast<std::uint8_t>(
                (packed>>((i%2u)*4u))&0x0Fu);
        teams[i]=team<6 ? team : 0xFF;
    }
}

void ObserveExtendedTeams(const RkNetRoomState& room,
                          TeamRuntimeCache& cache) noexcept {
    try {
        std::uint32_t packet=0;
        std::uint32_t size=0;
        if (!ReadCurrentTeamRoomPacket(room,packet,size)) return;

        const std::uint8_t messageType=Memory::Read8(packet);
        if (messageType==kExtendedTeamUpdateMessage &&
            size>=kPulRoomMinimumTeamSize) {
            cache.extendedActive=true;
            ReadExtendedTeamsFromPacket(packet,cache.extendedTeams);
            return;
        }

        if (messageType!=1 || size<kPulRoomMinimumTeamSize) return;

        const std::uint64_t hostContext=
            Memory::Read64(packet+kPulRoomHostSystemContext);
        cache.extendedActive=
            (hostContext&(1ull<<kExtendedTeamsContextBit))!=0;
        if (!cache.extendedActive) {
            cache.extendedTeams.fill(0xFF);
            return;
        }

        const std::uint16_t message=
            static_cast<std::uint16_t>(
                (static_cast<std::uint16_t>(
                    Memory::Read8(packet+1))<<8u) |
                Memory::Read8(packet+2));
        if (message==0 || message==2 || message==3) {
            ReadExtendedTeamsFromPacket(packet,cache.extendedTeams);
        }
    } catch (...) {
    }
}

bool ReadVanillaTeams(
    std::array<std::uint8_t,12>& teams) noexcept {
    try {
        if (!Memory::Contains(kRacedataInstance,4)) return false;
        const std::uint32_t racedata=Memory::Read32(kRacedataInstance);
        if (racedata==0) return false;

        const std::array<std::uint32_t,2> scenarios={
            racedata+kRacedataRacesScenario,
            racedata+kRacedataMenusScenario,
        };

        for (const std::uint32_t scenario:scenarios) {
            if (!Memory::Contains(
                    scenario,
                    kScenarioSettings+kSettingsModeFlags+4)) continue;

            const std::uint32_t modeFlags=
                Memory::Read32(
                    scenario+kScenarioSettings+kSettingsModeFlags);
            if ((modeFlags&kTeamsEnabledFlag)==0) continue;

            const std::uint8_t playerCount=
                Memory::Read8(scenario+kScenarioPlayerCount);
            if (playerCount==0 || playerCount>12) continue;

            teams.fill(0xFF);
            bool any=false;
            for (std::uint32_t i=0;i<playerCount;++i) {
                const std::uint32_t team=
                    Memory::Read32(
                        scenario+kScenarioPlayers+i*kPlayerSize+kPlayerTeam);
                if (team<2) {
                    teams[i]=static_cast<std::uint8_t>(team);
                    any=true;
                }
            }
            if (any) return true;
        }
    } catch (...) {
    }

    teams.fill(0xFF);
    return false;
}

TeamMembershipSnapshot ReadTeamMembership(
    std::uint64_t identityGeneration,
    bool localRoomActive) noexcept {
    TeamMembershipSnapshot result;
    auto& cache=TeamCache();
    if (!localRoomActive) {
        ResetTeamCache(cache,identityGeneration,0);
        return result;
    }

    RkNetRoomState room;
    if (!ReadRkNetRoomState(room)) return result;

    if (cache.identityGeneration!=identityGeneration ||
        cache.groupId!=room.groupId) {
        ResetTeamCache(cache,identityGeneration,room.groupId);
    }

    ObserveExtendedTeams(room,cache);

    std::array<std::uint8_t,12> teams=cache.extendedTeams;
    if (cache.extendedActive) {
        result.active=true;
    } else if (ReadVanillaTeams(teams)) {
        result.active=true;
    }
    if (!result.active) return result;

    std::array<bool,6> localTeams{};
    std::array<bool,12> aidKnown{};
    std::array<bool,12> aidTeammate{};
    std::array<std::uint8_t,12> playerAids{};
    playerAids.fill(0xFF);

    for (std::uint32_t player=0;player<12;++player) {
        const std::uint8_t team=teams[player];
        if (team>=localTeams.size()) continue;
        const std::uint8_t aid=
            Memory::Read8(
                room.controller+kControllerPlayerAidMap+player);
        playerAids[player]=aid;
        if (aid>=12) continue;
        aidKnown[aid]=true;
        if (aid==room.localAid) localTeams[team]=true;
    }

    bool localTeamKnown=false;
    for (const bool present:localTeams) {
        localTeamKnown=localTeamKnown || present;
    }
    if (!localTeamKnown) return result;

    for (std::uint32_t player=0;player<12;++player) {
        const std::uint8_t team=teams[player];
        const std::uint8_t aid=playerAids[player];
        if (team<localTeams.size() &&
            aid<12 &&
            localTeams[team]) aidTeammate[aid]=true;
    }

    if (!Memory::Contains(kDwcMatchControlInstance,4)) return result;
    const std::uint32_t matchControl=
        Memory::Read32(kDwcMatchControlInstance);
    if (matchControl==0 ||
        !Memory::Contains(
            matchControl+kDwcMatchNodes,
            kDwcNodeSize*kDwcNodeCount)) return result;

    std::unordered_set<std::string> known;
    std::unordered_set<std::string> teammates;
    for (std::uint32_t i=0;i<kDwcNodeCount;++i) {
        const std::uint32_t node=
            matchControl+kDwcMatchNodes+i*kDwcNodeSize;
        const std::uint32_t pid=
            Memory::Read32(node+kDwcNodePid);
        const std::uint8_t aid=
            Memory::Read8(node+kDwcNodeAid);
        if (pid==0 ||
            aid>=12 ||
            (room.availableAids&(1u<<aid))==0 ||
            !aidKnown[aid]) continue;

        const std::string profileId=std::to_string(pid);
        known.insert(profileId);
        if (aidTeammate[aid]) teammates.insert(profileId);
    }

    result.knownProfileIds.assign(known.begin(),known.end());
    result.teammateProfileIds.assign(teammates.begin(),teammates.end());
    return result;
}

std::vector<std::string> ReadFriendProfileIds() noexcept {
    std::vector<std::string> result;
    try {
        if(!RuntimeProduct::IsRetroRewind() ||
           !Memory::Contains(kRkNetFriendMgrInstance,4)) {
            return result;
        }

        const std::uint32_t manager=Memory::Read32(kRkNetFriendMgrInstance);
        const std::uint32_t bytes=kFriendMgrFriendCount*4u;
        if(manager==0 ||
           !Memory::Contains(manager+kFriendMgrFriendPids,bytes)) {
            return result;
        }

        result.reserve(kFriendMgrFriendCount);
        for(std::uint32_t i=0;i<kFriendMgrFriendCount;++i) {
            const std::uint32_t profileId=
                Memory::Read32(manager+kFriendMgrFriendPids+i*4u);
            if(profileId!=0) result.push_back(std::to_string(profileId));
        }
    } catch(...) {
        result.clear();
    }
    return result;
}

bool IsFriendProfileId(
    const std::vector<std::string>& friendProfileIds,
    const std::string& profileId) {
    return std::find(
        friendProfileIds.begin(),
        friendProfileIds.end(),
        profileId)!=friendProfileIds.end();
}

void ClearLocalRoomSnapshotLocked(RoomLookupState& state,
                                  std::uint64_t generation,
                                  std::string_view status) {
    RoomSnapshot cleared;
    cleared.lookupComplete = true;
    cleared.lookupSucceeded = true;
    cleared.localRoomActive = false;
    cleared.identityGeneration = generation;
    cleared.profileId = state.desiredProfileId;
    cleared.status = std::string(status);
    state.snapshot = std::move(cleared);
}


std::vector<std::string> SplitLines(const std::string& value) {
    std::vector<std::string> lines;
    std::istringstream stream(value);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(std::move(line));
    }
    return lines;
}

std::string DecodeHex(std::string_view value) {
    if (value.size() % 2 != 0) {
        return {};
    }
    std::string decoded;
    decoded.reserve(value.size() / 2);
    auto nibble = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < value.size(); i += 2) {
        const int hi = nibble(value[i]);
        const int lo = nibble(value[i + 1]);
        if (hi < 0 || lo < 0) {
            return {};
        }
        decoded.push_back(static_cast<char>((hi << 4) | lo));
    }
    return decoded;
}

RoomSnapshot ParseRoomReply(const std::string& message, std::string_view expectedProfileId) {
    RoomSnapshot result;
    result.lookupComplete = true;

    if (message.rfind("RR_DEBUG_FAIL ", 0) == 0) {
        result.status = message.substr(14);
        return result;
    }
    if (message.rfind("ERROR ", 0) == 0) {
        result.status = message.substr(6);
        return result;
    }
    constexpr std::string_view prefix = "RR_DEBUG_STATUS\n";
    if (message.rfind(prefix, 0) != 0) {
        result.status = "Unexpected signaling response";
        return result;
    }

    const std::vector<std::string> lines = SplitLines(message.substr(prefix.size()));
    if (lines.size() < 5 || lines[0] != expectedProfileId) {
        result.status = "Malformed Retro Rewind room response";
        return result;
    }

    result.profileId = lines[0];
    result.roomId = lines[1];
    result.roomInstanceId = lines[2];
    result.created = lines[3];

    std::size_t expectedPlayers = 0;
    const auto parsed = std::from_chars(lines[4].data(), lines[4].data() + lines[4].size(), expectedPlayers);
    if (parsed.ec != std::errc{} || parsed.ptr != lines[4].data() + lines[4].size() || expectedPlayers > 12) {
        result.status = "Malformed Retro Rewind roster count";
        return result;
    }

    for (std::size_t i = 5; i < lines.size() && result.players.size() < expectedPlayers; ++i) {
        const std::string& line = lines[i];
        const std::size_t first = line.find('\t');
        const std::size_t second = first == std::string::npos ? std::string::npos : line.find('\t', first + 1);
        if (first == std::string::npos || second == std::string::npos) {
            continue;
        }

        RoomPlayer player;
        player.profileId = line.substr(0, first);
        player.voiceChat = line.substr(first + 1, second - first - 1) == "1";
        player.name = DecodeHex(std::string_view(line).substr(second + 1));
        if (player.name.empty()) {
            player.name = "Player";
        }
        if (!player.profileId.empty()) {
            result.players.push_back(std::move(player));
        }
    }

    if (result.players.size() != expectedPlayers) {
        result.status = "Incomplete Retro Rewind roster response";
        return result;
    }

    // From here on the Worker reply was structurally valid. An empty room is a
    // real negative roster result, not a transport/parser failure.
    result.lookupSucceeded = true;

    if (result.roomId.empty()) {
        result.status = "Online, but not currently present in a public RR room";
        return result;
    }
    if (result.roomInstanceId.empty() || result.created.empty()) {
        result.lookupSucceeded = false;
        result.status = "Incomplete Retro Rewind room response";
        return result;
    }

    result.roomFound = true;
    result.status = "Room found through public RWFC roster (unverified)";
    return result;
}

#if defined(_WIN32)

constexpr auto kRoomPresenceRefreshInterval = std::chrono::seconds(30);
constexpr auto kRoomPresenceReplyTimeout = std::chrono::seconds(5);
constexpr auto kRoomReconnectDelay = std::chrono::seconds(5);

std::string WinHttpError(const char* operation, DWORD error = GetLastError()) {
    return std::string(operation) + " failed (WinHTTP " + std::to_string(error) + ")";
}

struct WinHttpHandle {
    HINTERNET value = nullptr;
    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET handle) : value(handle) {}
    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
    WinHttpHandle(WinHttpHandle&& other) noexcept : value(std::exchange(other.value, nullptr)) {}
    WinHttpHandle& operator=(WinHttpHandle&& other) noexcept {
        if (this != &other) {
            if (value) WinHttpCloseHandle(value);
            value = std::exchange(other.value, nullptr);
        }
        return *this;
    }
    ~WinHttpHandle() {
        if (value) WinHttpCloseHandle(value);
    }
};

void FinishReleaseCheck(
    bool productRequired,
    bool integrationRequired,
    bool protocolRequired,
    bool wiiCompiledRequired,
    std::uint32_t minimumProtocol,
    std::string latestVersion,
    std::string latestPatchRevision,
    std::string requiredWiiCompiledVersion,
    std::string status) {
    auto& state=ReleaseState();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.workerRunning=false;
    state.snapshot.checkStarted=true;
    state.snapshot.checkComplete=true;
    state.snapshot.checkSucceeded=true;
    state.snapshot.productUpdateRequired=productRequired;
    state.snapshot.integrationUpdateRequired=integrationRequired;
    state.snapshot.protocolUpdateRequired=protocolRequired;
    state.snapshot.wiiCompiledUpdateRequired=wiiCompiledRequired;
    state.snapshot.updateAvailable=
        productRequired ||
        integrationRequired ||
        protocolRequired ||
        wiiCompiledRequired;
    state.snapshot.minimumProtocol=minimumProtocol;
    state.snapshot.latestVersion=std::move(latestVersion);
    state.snapshot.latestPatchRevision=std::move(latestPatchRevision);
    state.snapshot.requiredWiiCompiledVersion=
        std::move(requiredWiiCompiledVersion);
    state.snapshot.status=std::move(status);
    state.nextCheck=
        std::chrono::steady_clock::now()+kReleaseCheckInterval;
}

void FinishReleaseCheckFailure(std::string status) {
    auto& state=ReleaseState();
    std::lock_guard<std::mutex> lock(state.mutex);
    const bool hadCompletedCheck=state.snapshot.checkComplete;
    state.workerRunning=false;
    state.snapshot.checkStarted=true;
    state.snapshot.checkComplete=true;
    state.snapshot.checkSucceeded=false;

    if(!hadCompletedCheck) {
        state.snapshot.updateAvailable=false;
        state.snapshot.productUpdateRequired=false;
        state.snapshot.integrationUpdateRequired=false;
        state.snapshot.protocolUpdateRequired=false;
        state.snapshot.wiiCompiledUpdateRequired=false;
        state.snapshot.minimumProtocol=0;
        state.snapshot.latestVersion.clear();
        state.snapshot.latestPatchRevision.clear();
        state.snapshot.requiredWiiCompiledVersion.clear();
        state.snapshot.status=std::move(status);
    } else if(!state.snapshot.updateAvailable) {
        state.snapshot.status=std::move(status);
    }

    state.nextCheck=
        std::chrono::steady_clock::now()+kReleaseCheckInterval;
}

void ReleaseCheckWorker() {
    constexpr wchar_t kHost[]=L"github.com";
    constexpr wchar_t kPath[]=
        L"/waxedhyphen/MKW-VoiceChat/releases/latest/download/mkwvc-release.json";

    WinHttpHandle session(WinHttpOpen(
        L"MKW VoiceChat release check/0.14",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0));
    if(!session.value) {
        FinishReleaseCheckFailure("Could not check for updates");
        return;
    }
    WinHttpSetTimeouts(session.value,5000,5000,5000,5000);

    WinHttpHandle connection(WinHttpConnect(
        session.value,
        kHost,
        INTERNET_DEFAULT_HTTPS_PORT,
        0));
    if(!connection.value) {
        FinishReleaseCheckFailure("Could not check for updates");
        return;
    }

    WinHttpHandle request(WinHttpOpenRequest(
        connection.value,
        L"GET",
        kPath,
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE));
    if(!request.value) {
        FinishReleaseCheckFailure("Could not check for updates");
        return;
    }

    constexpr wchar_t kHeaders[]=
        L"Cache-Control: no-cache\r\nAccept: application/json\r\n";
    if(!WinHttpSendRequest(
            request.value,
            kHeaders,
            static_cast<DWORD>(-1L),
            WINHTTP_NO_REQUEST_DATA,
            0,
            0,
            0) ||
       !WinHttpReceiveResponse(request.value,nullptr)) {
        FinishReleaseCheckFailure("Could not check for updates");
        return;
    }

    DWORD statusCode=0;
    DWORD statusSize=sizeof(statusCode);
    if(!WinHttpQueryHeaders(
            request.value,
            WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &statusCode,
            &statusSize,
            WINHTTP_NO_HEADER_INDEX)) {
        FinishReleaseCheckFailure("Could not check for updates");
        return;
    }
    if(statusCode==404) {
        FinishReleaseCheck(
            false,false,false,false,0,
            {},{},{},
            "No published release yet");
        return;
    }
    if(statusCode!=200) {
        FinishReleaseCheckFailure("Could not check for updates");
        return;
    }

    std::string json;
    while(json.size()<32768) {
        DWORD available=0;
        if(!WinHttpQueryDataAvailable(request.value,&available)) {
            FinishReleaseCheckFailure("Could not check for updates");
            return;
        }
        if(available==0) break;
        const DWORD chunk=
            static_cast<DWORD>(
                std::min<std::size_t>(
                    available,
                    32768-json.size()));
        std::string buffer(chunk,'\0');
        DWORD read=0;
        if(!WinHttpReadData(
                request.value,
                buffer.data(),
                chunk,
                &read)) {
            FinishReleaseCheckFailure("Could not check for updates");
            return;
        }
        buffer.resize(read);
        json+=buffer;
        if(read==0) break;
    }

    const std::string latest=JsonStringValue(json,"version");
    const std::string latestPatchRevision=
        JsonStringValue(json,"patchRevision");
    const std::string requiredWiiCompiledVersion=
        JsonStringValue(json,"wiiCompiledVersion");
    int schema=0;
    int minimumProtocol=0;
    if(!JsonIntValue(json,"schemaVersion",schema) ||
       schema!=1 ||
       !JsonIntValue(json,"minimumProtocol",minimumProtocol) ||
       minimumProtocol<1 ||
       latest.empty() ||
       latestPatchRevision.empty() ||
       requiredWiiCompiledVersion.empty()) {
        FinishReleaseCheckFailure("Update manifest is invalid");
        return;
    }

    std::array<int,3> parsed{};
    std::array<int,3> parsedWiiCompiled{};
    if(!ParseVersionTriplet(latest,parsed) ||
       !ParseVersionTriplet(
           requiredWiiCompiledVersion,
           parsedWiiCompiled)) {
        FinishReleaseCheckFailure("Update manifest has invalid version");
        return;
    }

    const int versionComparison=
        CompareVersions(latest,kMkwVoiceChatVersion);
    const bool releaseNotOlder=versionComparison>=0;
    const bool productRequired=versionComparison>0;
    const bool protocolRequired=
        releaseNotOlder &&
        minimumProtocol>
        static_cast<int>(kMkwVoiceChatProtocolVersion);
    const bool wiiCompiledRequired=
        releaseNotOlder &&
        CompareVersions(
            requiredWiiCompiledVersion,
            kMkwVoiceChatWiiCompiledVersion)!=0;
    const bool integrationRequired=
        versionComparison==0 &&
        latestPatchRevision!=kMkwVoiceChatPatchRevision;
    const bool updateRequired=
        productRequired ||
        integrationRequired ||
        protocolRequired ||
        wiiCompiledRequired;

    FinishReleaseCheck(
        productRequired,
        integrationRequired,
        protocolRequired,
        wiiCompiledRequired,
        static_cast<std::uint32_t>(minimumProtocol),
        latest,
        latestPatchRevision,
        requiredWiiCompiledVersion,
        updateRequired ? "Update required" : "Current");
}

void EnsureReleaseCheckStarted() {
    auto& state=ReleaseState();
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if(state.workerRunning) return;

        const auto now=std::chrono::steady_clock::now();
        const bool initial=!state.snapshot.checkStarted;
        const bool periodic=
            state.snapshot.checkComplete &&
            state.nextCheck!=std::chrono::steady_clock::time_point{} &&
            now>=state.nextCheck;
        if(!initial && !periodic) return;

        state.snapshot.checkStarted=true;
        if(!state.snapshot.checkComplete) {
            state.snapshot.status="Checking for updates...";
        }
        state.workerRunning=true;
    }

    std::thread([] {
        ReleaseCheckWorker();
    }).detach();
}

DWORD SendWebSocketText(HINTERNET socket, const std::string& message) {
    return WinHttpWebSocketSend(
        socket,
        WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
        const_cast<char*>(message.data()),
        static_cast<DWORD>(message.size()));
}

DWORD ReceiveWebSocketText(HINTERNET socket, std::string& message, bool& closed) {
    message.clear();
    closed = false;

    while (message.size() <= kMaxRoomReplyBytes) {
        std::array<char, 4096> buffer{};
        DWORD bytesRead = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type = WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;
        const DWORD error = WinHttpWebSocketReceive(
            socket,
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            &bytesRead,
            &type);
        if (error != ERROR_SUCCESS) {
            return error;
        }
        if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
            closed = true;
            return ERROR_SUCCESS;
        }
        if (type != WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE &&
            type != WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) {
            return ERROR_INVALID_DATA;
        }

        message.append(buffer.data(), bytesRead);
        if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) {
            return message.size() <= kMaxRoomReplyBytes ? ERROR_SUCCESS : ERROR_INSUFFICIENT_BUFFER;
        }
    }

    return ERROR_INSUFFICIENT_BUFFER;
}

void ApplyRoomResultLocked(RoomLookupState& state, RoomSnapshot result,
                           std::uint64_t generation) {
    result.lookupInFlight = false;
    result.identityGeneration = generation;

    if (!state.desiredOnline || state.desiredIdentityGeneration != generation) {
        return;
    }

    if (!state.desiredRoomActive) {
        ClearLocalRoomSnapshotLocked(
            state, generation, "Not currently connected to an RKNet room");
        return;
    }

    result.localRoomActive = true;

    const bool hadConfirmedRoom =
        state.snapshot.roomFound &&
        state.snapshot.identityGeneration == generation;

    if (result.lookupSucceeded) {
        // A structurally valid Worker roster result is real room-state evidence.
        // If our PID is no longer in a room, leave the voice room immediately
        // instead of keeping stale peers audible in the lobby. This is distinct
        // from a transport failure below (for example a Wi-Fi interruption),
        // where no valid roster result exists and the last confirmed room is
        // deliberately preserved for reconnect.
        state.snapshot = std::move(result);
    } else if (hadConfirmedRoom) {
        // Worker/network/parser failures are not proof that the player left the
        // RR room. Preserve the last confirmed room while signaling reconnects.
        state.snapshot.lookupInFlight = false;
        state.snapshot.lookupComplete = true;
        state.snapshot.status =
            "Room preserved while voice signaling reconnects";
    } else {
        state.snapshot = std::move(result);
    }
}

void FinishRoomWorker(std::uint64_t generation, std::string status,
                      bool reconnect) {
    RoomLookupState& state = RoomState();
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.workerIdentityGeneration != generation) {
            return;
        }

        state.workerRunning = false;
        state.workerIdentityGeneration = UINT64_MAX;
        state.snapshot.lookupInFlight = false;
        state.snapshot.signalingConnected = false;

        if (state.desiredOnline && state.desiredIdentityGeneration == generation) {
            if (!status.empty()) {
                state.snapshot.lookupComplete = true;
                if (state.snapshot.roomFound) {
                    state.snapshot.status =
                        "Room still active; signaling connection will reconnect";
                } else {
                    state.snapshot.status = std::move(status);
                }
            }
            state.nextReconnectAttempt = reconnect
                ? std::chrono::steady_clock::now() + kRoomReconnectDelay
                : std::chrono::steady_clock::time_point{};
        } else {
            state.nextReconnectAttempt = {};
        }
    }
    state.wake.notify_all();
}

void PersistentRoomWorker(std::string profileId, std::uint64_t generation) {
    constexpr wchar_t kWorkerHost[] = L"mkw-voicechat-signaling.mkwvoicechat.workers.dev";

    WinHttpHandle session(WinHttpOpen(
        L"MKW VoiceChat WiiCompiled bridge/0.2",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0));
    if (!session.value) {
        FinishRoomWorker(generation, WinHttpError("WinHttpOpen"), true);
        return;
    }

    // Connection setup should fail quickly, while the receive side may remain
    // blocked because the sender refreshes presence every 30 seconds.
    WinHttpSetTimeouts(session.value, 5000, 5000, 5000, 60000);

    WinHttpHandle connection(WinHttpConnect(
        session.value, kWorkerHost, INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!connection.value) {
        FinishRoomWorker(generation, WinHttpError("WinHttpConnect"), true);
        return;
    }

    WinHttpHandle request(WinHttpOpenRequest(
        connection.value,
        L"GET",
        L"/",
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE));
    if (!request.value) {
        FinishRoomWorker(generation, WinHttpError("WinHttpOpenRequest"), true);
        return;
    }

    DWORD error = WinHttpSetOption(
        request.value, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)
        ? ERROR_SUCCESS : GetLastError();
    if (error != ERROR_SUCCESS) {
        FinishRoomWorker(generation, WinHttpError("WebSocket upgrade", error), true);
        return;
    }
    if (!WinHttpSendRequest(request.value, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        FinishRoomWorker(generation, WinHttpError("WinHttpSendRequest"), true);
        return;
    }
    if (!WinHttpReceiveResponse(request.value, nullptr)) {
        FinishRoomWorker(generation, WinHttpError("WinHttpReceiveResponse"), true);
        return;
    }

    WinHttpHandle socket(WinHttpWebSocketCompleteUpgrade(request.value, 0));
    if (!socket.value) {
        FinishRoomWorker(generation, WinHttpError("WinHttpWebSocketCompleteUpgrade"), true);
        return;
    }

    DWORD keepAliveMs = 30000;
    (void)WinHttpSetOption(
        socket.value,
        WINHTTP_OPTION_WEB_SOCKET_KEEPALIVE_INTERVAL,
        &keepAliveMs,
        sizeof(keepAliveMs));
    DWORD closeTimeoutMs = 5000;
    (void)WinHttpSetOption(
        socket.value,
        WINHTTP_OPTION_WEB_SOCKET_CLOSE_TIMEOUT,
        &closeTimeoutMs,
        sizeof(closeTimeoutMs));

    RoomLookupState& state = RoomState();
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.desiredOnline &&
            state.desiredIdentityGeneration == generation) {
            state.snapshot.signalingConnected = true;
            state.snapshot.status = state.desiredRoomActive
                ? "Voice signaling connected; registering presence..."
                : "Voice signaling connected; waiting for RKNet room";
        }
    }
    state.wake.notify_all();

    std::atomic<bool> connectionAlive{true};
    std::atomic<DWORD> sendFailure{ERROR_SUCCESS};
    std::atomic<std::uint64_t> receivedReplies{0};
    std::atomic<bool> roomReplyTimedOut{false};

    // WinHTTP explicitly permits one WebSocket sender and one receiver to run
    // concurrently. The receiver below consumes Worker-pushed presence updates;
    // this sender handles the initial registration and periodic verification
    // without touching the game/UI thread.
    std::thread sender([&]() {
        bool presenceRegistered = false;

        const auto sendPresence = [&]() -> bool {
            std::string command;
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                if (!state.desiredOnline ||
                    state.desiredIdentityGeneration != generation ||
                    !connectionAlive.load(std::memory_order_acquire)) {
                    return false;
                }

                if (state.desiredRoomActive) {
                    state.snapshot.lookupInFlight = true;
                    state.snapshot.localRoomActive = true;
                    state.snapshot.identityGeneration = generation;
                    state.snapshot.profileId = profileId;
                    if (!state.snapshot.lookupComplete && !state.snapshot.roomFound) {
                        state.snapshot.status = "Verifying Retro Rewind voice presence...";
                    }
                    command = "RR_DEBUG_PRESENCE " + profileId;
                } else if (presenceRegistered) {
                    command = "RR_DEBUG_PRESENCE_CLEAR";
                } else {
                    return true;
                }
            }

            const DWORD sendError = SendWebSocketText(socket.value, command);
            if (sendError != ERROR_SUCCESS) {
                sendFailure.store(sendError, std::memory_order_release);
                connectionAlive.store(false, std::memory_order_release);
                state.wake.notify_all();
                (void)WinHttpWebSocketShutdown(
                    socket.value,
                    WINHTTP_WEB_SOCKET_ENDPOINT_TERMINATED_CLOSE_STATUS,
                    nullptr,
                    0);
                return false;
            }

            presenceRegistered = command.rfind("RR_DEBUG_PRESENCE ", 0) == 0;
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                if (state.desiredIdentityGeneration == generation) {
                    state.snapshot.signalingConnected = true;
                    state.snapshot.presenceFrameSent = presenceRegistered;
                    if (!presenceRegistered) {
                        state.snapshot.signalingReplyReceived = false;
                    }
                }
            }
            state.wake.notify_all();
            return true;
        };

        const auto waitForRoomReply = [&](std::uint64_t before) -> bool {
            std::unique_lock<std::mutex> lock(state.mutex);
            state.wake.wait_for(lock, kRoomPresenceReplyTimeout, [&]() {
                return !connectionAlive.load(std::memory_order_acquire) ||
                       !state.desiredOnline ||
                       state.desiredIdentityGeneration != generation ||
                       !state.desiredRoomActive ||
                       receivedReplies.load(std::memory_order_acquire) != before;
            });
            return receivedReplies.load(std::memory_order_acquire) != before;
        };

        const std::uint64_t firstReply = receivedReplies.load(std::memory_order_acquire);
        if (!sendPresence()) {
            return;
        }

        // A second client can already see this PID as [Voice Chat] as soon as
        // RR_DEBUG_PRESENCE reached the Worker. If the local socket never
        // consumes the matching RR_DEBUG_STATUS, do one explicit lookup and
        // then reconnect instead of sitting forever on "verifying presence".
        if (presenceRegistered && !waitForRoomReply(firstReply)) {
            bool stillWaiting = false;
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                stillWaiting =
                    connectionAlive.load(std::memory_order_acquire) &&
                    state.desiredOnline &&
                    state.desiredIdentityGeneration == generation &&
                    state.desiredRoomActive;
                if (stillWaiting) {
                    state.snapshot.status =
                        "Voice presence registered; room reply delayed, retrying...";
                }
            }

            if (stillWaiting) {
                const std::uint64_t retryReply =
                    receivedReplies.load(std::memory_order_acquire);
                const std::string retryCommand =
                    "RR_DEBUG_LOOKUP " + profileId;
                const DWORD retryError =
                    SendWebSocketText(socket.value, retryCommand);
                if (retryError != ERROR_SUCCESS) {
                    sendFailure.store(retryError, std::memory_order_release);
                    connectionAlive.store(false, std::memory_order_release);
                    state.wake.notify_all();
                    (void)WinHttpWebSocketShutdown(
                        socket.value,
                        WINHTTP_WEB_SOCKET_ENDPOINT_TERMINATED_CLOSE_STATUS,
                        nullptr,
                        0);
                    return;
                }

                if (!waitForRoomReply(retryReply)) {
                    {
                        std::lock_guard<std::mutex> lock(state.mutex);
                        stillWaiting =
                            connectionAlive.load(std::memory_order_acquire) &&
                            state.desiredOnline &&
                            state.desiredIdentityGeneration == generation &&
                            state.desiredRoomActive;
                        if (stillWaiting) {
                            state.snapshot.status =
                                "Voice room reply timed out; reconnecting signaling...";
                        }
                    }

                    if (stillWaiting) {
                        roomReplyTimedOut.store(true, std::memory_order_release);
                        connectionAlive.store(false, std::memory_order_release);
                        state.wake.notify_all();
                        (void)WinHttpWebSocketShutdown(
                            socket.value,
                            WINHTTP_WEB_SOCKET_ENDPOINT_TERMINATED_CLOSE_STATUS,
                            nullptr,
                            0);
                        return;
                    }
                }
            }
        }

        while (connectionAlive.load(std::memory_order_acquire)) {
            bool stop = false;
            {
                std::unique_lock<std::mutex> lock(state.mutex);
                const bool registeredBeforeWait = presenceRegistered;
                state.wake.wait_for(lock, kRoomPresenceRefreshInterval, [&]() {
                    return !connectionAlive.load(std::memory_order_acquire) ||
                           !state.desiredOnline ||
                           state.desiredIdentityGeneration != generation ||
                           state.desiredRoomActive != registeredBeforeWait;
                });

                stop =
                    !connectionAlive.load(std::memory_order_acquire) ||
                    !state.desiredOnline ||
                    state.desiredIdentityGeneration != generation;
            }

            if (stop) {
                break;
            }
            if (!sendPresence()) {
                return;
            }
        }

        if (connectionAlive.load(std::memory_order_acquire)) {
            if (presenceRegistered) {
                (void)SendWebSocketText(socket.value, "RR_DEBUG_PRESENCE_CLEAR");
            }
            (void)WinHttpWebSocketShutdown(
                socket.value,
                WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS,
                nullptr,
                0);
            connectionAlive.store(false, std::memory_order_release);
        }
    });

    std::string terminalStatus;
    bool reconnect = true;

    while (connectionAlive.load(std::memory_order_acquire)) {
        std::string message;
        bool closed = false;
        error = ReceiveWebSocketText(socket.value, message, closed);
        if (error != ERROR_SUCCESS) {
            terminalStatus = WinHttpError("WinHttpWebSocketReceive", error);
            connectionAlive.store(false, std::memory_order_release);
            state.wake.notify_all();
            break;
        }
        if (closed) {
            connectionAlive.store(false, std::memory_order_release);
            state.wake.notify_all();
            terminalStatus = "Voice signaling WebSocket closed";
            break;
        }

        RoomSnapshot result = ParseRoomReply(message, profileId);
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            result.signalingConnected = true;
            result.presenceFrameSent = state.snapshot.presenceFrameSent;
            result.signalingReplyReceived = true;
            ApplyRoomResultLocked(state, std::move(result), generation);
        }
        receivedReplies.fetch_add(1, std::memory_order_release);
        state.wake.notify_all();
    }

    state.wake.notify_all();
    if (sender.joinable()) {
        sender.join();
    }

    const DWORD failedSend = sendFailure.load(std::memory_order_acquire);
    if (failedSend != ERROR_SUCCESS) {
        terminalStatus = WinHttpError("WinHttpWebSocketSend", failedSend);
    } else if (roomReplyTimedOut.load(std::memory_order_acquire)) {
        terminalStatus = "Voice signaling room reply timed out";
    }

    {
        std::lock_guard<std::mutex> lock(state.mutex);
        // Identity change/offline is an intentional stop. The next identity is
        // started by ServiceRoomLookup without reporting a transport failure.
        if (!state.desiredOnline || state.desiredIdentityGeneration != generation) {
            reconnect = false;
            terminalStatus.clear();
        }
    }

    FinishRoomWorker(generation, std::move(terminalStatus), reconnect);
}

#else

void EnsureReleaseCheckStarted() {
    auto& state=ReleaseState();
    std::lock_guard<std::mutex> lock(state.mutex);
    if(state.snapshot.checkStarted) return;
    state.snapshot.checkStarted=true;
    state.snapshot.checkComplete=true;
    state.snapshot.status="Update check unavailable on this platform";
}

constexpr auto kRoomReconnectDelay = std::chrono::seconds(5);

void FinishRoomWorker(std::uint64_t generation, std::string status,
                      bool reconnect) {
    RoomLookupState& state = RoomState();
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.workerIdentityGeneration != generation) {
            return;
        }

        state.workerRunning = false;
        state.workerIdentityGeneration = UINT64_MAX;
        state.snapshot.lookupInFlight = false;

        if (state.desiredOnline && state.desiredIdentityGeneration == generation) {
            if (!status.empty()) {
                state.snapshot.lookupComplete = true;
                if (state.snapshot.roomFound) {
                    state.snapshot.status =
                        "Room still active; signaling connection will reconnect";
                } else {
                    state.snapshot.status = std::move(status);
                }
            }
            state.nextReconnectAttempt = reconnect
                ? std::chrono::steady_clock::now() + kRoomReconnectDelay
                : std::chrono::steady_clock::time_point{};
        } else {
            state.nextReconnectAttempt = {};
        }
    }
    state.wake.notify_all();
}

void PersistentRoomWorker(std::string, std::uint64_t generation) {
    FinishRoomWorker(generation,
                     "RR signaling presence is not implemented on this host platform yet",
                     false);
}

#endif

} // namespace

void ObserveGpcmSend(std::uint32_t wiiFd, std::uint16_t peerPort,
                     const std::uint8_t* data, std::size_t size) noexcept {
    if (!ShouldObserve(peerPort)) {
        return;
    }

    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        SocketState& state = g_sockets[wiiFd];
        FeedLocked(wiiFd, state.outbound, data, size, HandleOutboundLocked);
    } catch (...) {
        // Voice integration is observational. It must never break the game's
        // networking path if host allocation/parsing fails.
    }
}

void ObserveGpcmReceive(std::uint32_t wiiFd, std::uint16_t peerPort,
                        const std::uint8_t* data, std::size_t size) noexcept {
    if (!ShouldObserve(peerPort)) {
        return;
    }

    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        SocketState& state = g_sockets[wiiFd];
        FeedLocked(wiiFd, state.inbound, data, size, HandleInboundLocked);
    } catch (...) {
        // See ObserveGpcmSend: the bridge must remain side-effect free.
    }
}

void OnSocketClosed(std::uint32_t wiiFd, std::uint16_t peerPort) noexcept {
    (void)peerPort;
    if (!RuntimeProduct::IsRetroRewind()) {
        return;
    }

    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_sockets.erase(wiiFd);
        if (g_identitySocket == wiiFd) {
            ClearIdentityLocked();
        }
    } catch (...) {
    }
}

IdentitySnapshot Snapshot() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_identity;
}

void ServiceRoomLookup() noexcept {
    try {
        EnsureReleaseCheckStarted();

        ReleaseStatus release;
        {
            auto& releaseState=ReleaseState();
            std::lock_guard<std::mutex> lock(releaseState.mutex);
            release=releaseState.snapshot;
        }

        const bool waitingForInitialCheck=
            !release.checkComplete;
        const bool updateRequired=
            release.checkComplete &&
            release.updateAvailable;
        mkwvc::setEmbeddedVoiceRuntimeBlocked(
            waitingForInitialCheck || updateRequired,
            waitingForInitialCheck
                ? "Checking for MKW Voice Chat updates..."
                : (updateRequired
                    ? "MKW Voice Chat update required"
                    : std::string{}));

        const IdentitySnapshot identity=Snapshot();
        const bool localRoomActive=
            identity.online &&
            !identity.profileId.empty() &&
            IsLocalRkNetRoomActive();

        const TeamMembershipSnapshot teams=
            ReadTeamMembership(identity.generation,localRoomActive);

        mkwvc::EmbeddedVoiceSessionInput voiceInput;
        voiceInput.localRoomActive=localRoomActive;
        voiceInput.profileId=identity.profileId;
        voiceInput.sessionKey=identity.sessionKey;
        voiceInput.gameName=identity.gameName;
        voiceInput.friendProfileIds=ReadFriendProfileIds();
        voiceInput.roomProfileIds=
            ReadRoomProfileIds(identity.profileId,localRoomActive);
        voiceInput.teamModeActive=teams.active;
        voiceInput.teamProfileIds=teams.knownProfileIds;
        voiceInput.teammateProfileIds=teams.teammateProfileIds;
        voiceInput.identityGeneration=identity.generation;
        mkwvc::serviceEmbeddedVoiceSession(voiceInput);
    } catch(...) {
    }
}

RoomSnapshot Room() {
    const IdentitySnapshot identity=Snapshot();
    const auto session=mkwvc::embeddedVoiceSessionStatus();
    const auto friendProfileIds=ReadFriendProfileIds();

    RoomSnapshot room;
    room.localRoomActive=
        identity.online &&
        !identity.profileId.empty() &&
        IsLocalRkNetRoomActive();
    room.lookupInFlight=session.developmentAdmissionPending;
    room.lookupComplete=session.developmentAdmitted || !session.status.empty();
    room.lookupSucceeded=session.roomFound;
    room.roomFound=room.localRoomActive && session.roomFound;
    room.signalingConnected=session.signalingConnected;
    room.presenceFrameSent=
        session.developmentAdmissionPending ||
        session.developmentAdmitted;
    room.signalingReplyReceived=
        session.developmentAdmitted ||
        (!session.developmentAdmissionPending &&
         session.lifecycleActive &&
         session.signalingConnected &&
         !session.status.empty());
    room.status=session.status;
    room.profileId=identity.profileId;
    room.roomId=session.roomId;
    room.roomInstanceId=session.roomInstanceId;
    room.created=session.roomCreated;
    room.localRoomProfileIds=
        ReadRoomProfileIds(identity.profileId,room.localRoomActive);
    room.identityGeneration=identity.generation;
    room.players.reserve(session.roomPlayers.size());

    for(const auto& source:session.roomPlayers) {
        RoomPlayer player;
        player.profileId=source.profileId;
        player.name=source.displayName;
        player.friendCode=source.friendCode;
        player.voiceChat=source.voiceChat;
        player.isFriend=IsFriendProfileId(friendProfileIds,source.profileId);
        room.players.push_back(std::move(player));
    }

    return room;
}

ReleaseStatus Release() {
    EnsureReleaseCheckStarted();
    auto& state=ReleaseState();
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.snapshot;
}

bool LaunchInstalledUpdater() noexcept {
#if defined(_WIN32)
    try {
        std::array<wchar_t,32768> modulePath{};
        const DWORD length=GetModuleFileNameW(
            nullptr,
            modulePath.data(),
            static_cast<DWORD>(modulePath.size()));
        if(length==0 || length>=modulePath.size()) return false;

        std::wstring path(modulePath.data(),length);
        const auto executableSlash=path.find_last_of(L"\\/");
        if(executableSlash==std::wstring::npos) return false;
        path.resize(executableSlash);

        const auto productSlash=path.find_last_of(L"\\/");
        if(productSlash==std::wstring::npos) return false;
        path.resize(productSlash+1);
        path+=L"MKWVoiceChat-Updater.exe";

        const DWORD attributes=GetFileAttributesW(path.c_str());
        if(attributes==INVALID_FILE_ATTRIBUTES ||
           (attributes&FILE_ATTRIBUTE_DIRECTORY)!=0) {
            auto& state=ReleaseState();
            std::lock_guard<std::mutex> lock(state.mutex);
            state.snapshot.status=
                "Installed bootstrap updater is missing; reinstall MKW Voice Chat";
            return false;
        }

        std::wstring command=L"\""+path+L"\" --wait-pid "+
            std::to_wstring(GetCurrentProcessId());
        std::vector<wchar_t> writable(command.begin(),command.end());
        writable.push_back(L'\0');

        STARTUPINFOW startup{};
        startup.cb=sizeof(startup);
        PROCESS_INFORMATION process{};
        const BOOL started=CreateProcessW(
            path.c_str(),
            writable.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NEW_CONSOLE,
            nullptr,
            nullptr,
            &startup,
            &process);
        if(!started) return false;

        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);

        auto& state=ReleaseState();
        std::lock_guard<std::mutex> lock(state.mutex);
        state.snapshot.status=
            "Updater started; waiting for game exit";
        return true;
    } catch(...) {
        return false;
    }
#else
    return false;
#endif
}

} // namespace RetroRewindVoiceBridge
