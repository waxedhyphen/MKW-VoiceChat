#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace RetroRewindVoiceBridge {

inline constexpr const char* kMkwVoiceChatVersion =
    "@MKWVC_PRODUCT_VERSION@";
inline constexpr const char* kMkwVoiceChatPatchRevision =
    "@MKWVC_PATCH_REVISION@";
inline constexpr std::uint32_t kMkwVoiceChatProtocolVersion =
    @MKWVC_PROTOCOL_VERSION@;
inline constexpr const char* kMkwVoiceChatWiiCompiledVersion =
    "@MKWVC_WIICOMPILED_VERSION@";

struct ReleaseStatus {
    bool checkStarted = false;
    bool checkComplete = false;
    bool checkSucceeded = false;
    bool updateAvailable = false;
    bool productUpdateRequired = false;
    bool integrationUpdateRequired = false;
    bool protocolUpdateRequired = false;
    bool wiiCompiledUpdateRequired = false;
    std::uint32_t minimumProtocol = 0;
    std::string latestVersion;
    std::string latestPatchRevision;
    std::string requiredWiiCompiledVersion;
    std::string status;
};

// Host-only snapshot of the live Retro Rewind GPCM identity observed by the
// WiiCompiled networking HLE. The session key is intentionally memory-only:
// callers must never persist or log it.
struct IdentitySnapshot {
    bool online = false;
    std::string profileId;
    std::string sessionKey;
    std::string gameName;
    std::uint64_t generation = 0;
};

void ObserveGpcmSend(std::uint32_t wiiFd, std::uint16_t peerPort,
                     const std::uint8_t* data, std::size_t size) noexcept;
void ObserveGpcmReceive(std::uint32_t wiiFd, std::uint16_t peerPort,
                        const std::uint8_t* data, std::size_t size) noexcept;
void OnSocketClosed(std::uint32_t wiiFd, std::uint16_t peerPort) noexcept;

IdentitySnapshot Snapshot();

// Room players are resolved by the signaling Worker from the public RWFC
// roster. Voice admission currently trusts the client-supplied profile ID
// (unverified development admission) until a trusted RR verifier exists.
struct RoomPlayer {
    std::string profileId;
    std::string name;
    std::string friendCode;
    bool voiceChat = false;
    bool isFriend = false;
};

struct RoomSnapshot {
    bool lookupInFlight = false;
    bool lookupComplete = false;
    bool lookupSucceeded = false;
    bool localRoomActive = false;
    bool roomFound = false;
    bool signalingConnected = false;
    bool presenceFrameSent = false;
    bool signalingReplyReceived = false;
    std::string status;
    std::string profileId;
    std::string roomId;
    std::string roomInstanceId;
    std::string created;
    std::vector<std::string> localRoomProfileIds;
    std::vector<RoomPlayer> players;
    std::uint64_t identityGeneration = 0;
};

// Called from the host runtime once per presented frame. A live GPCM identity
// keeps one persistent signaling WebSocket alive in a background worker. Local
// RKNet room state gates voice presence immediately: leaving the game room
// clears voice presence without waiting for the public roster poll. Worker-
// pushed presence updates are consumed without network I/O on the game/UI thread.
void ServiceRoomLookup() noexcept;
RoomSnapshot Room();

ReleaseStatus Release();
bool LaunchInstalledUpdater() noexcept;

} // namespace RetroRewindVoiceBridge
