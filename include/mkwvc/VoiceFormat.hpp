#pragma once

#include <cstddef>
#include <cstdint>

namespace mkwvc {

struct VoiceFormat {
    static constexpr std::uint32_t SampleRate=48000;
    static constexpr std::uint32_t Channels=1;
    static constexpr std::uint32_t FrameDurationMs=20;
    static constexpr std::size_t FrameSamples=960;
    static constexpr std::size_t PacketHeaderBytes=18;
    static constexpr std::size_t MaxOpusPacketBytes=1275;
    static constexpr std::size_t MaxVoicePacketBytes=PacketHeaderBytes+MaxOpusPacketBytes;
};

}
