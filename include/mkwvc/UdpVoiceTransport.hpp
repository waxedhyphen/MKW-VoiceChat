#pragma once

#include "mkwvc/VoiceTransport.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace mkwvc {

class UdpVoiceTransport final : public VoiceTransport {
public:
    UdpVoiceTransport(std::string peerAddress,std::uint16_t port);
    ~UdpVoiceTransport();

    UdpVoiceTransport(const UdpVoiceTransport&)=delete;
    UdpVoiceTransport& operator=(const UdpVoiceTransport&)=delete;

    bool send(std::span<const std::byte> packet) override;
    std::size_t receive(std::span<std::byte> packet) override;
    std::string localAddress() const override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}
