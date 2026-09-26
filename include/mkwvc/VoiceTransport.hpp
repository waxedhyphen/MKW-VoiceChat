#pragma once

#include <cstddef>
#include <span>
#include <string>

namespace mkwvc {

class VoiceTransport {
public:
    virtual ~VoiceTransport()=default;

    VoiceTransport(const VoiceTransport&)=delete;
    VoiceTransport& operator=(const VoiceTransport&)=delete;

    virtual bool send(std::span<const std::byte> packet)=0;
    virtual std::size_t receive(std::span<std::byte> packet)=0;
    virtual std::string localAddress() const=0;

protected:
    VoiceTransport()=default;
};

}
