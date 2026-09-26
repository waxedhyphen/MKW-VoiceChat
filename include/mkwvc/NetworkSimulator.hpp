#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace mkwvc {

struct NetworkSimulationSettings {
    bool enabled=false;
    int latencyMs=0;
    int jitterMs=0;
    float packetLossPercent=0.0f;
    int burstLossPackets=1;
    float duplicatePercent=0.0f;
    float reorderPercent=0.0f;

    bool operator==(const NetworkSimulationSettings&) const = default;
};

struct NetworkSimulationEvent {
    std::uint32_t dropped=0;
    std::uint32_t duplicated=0;
    std::uint32_t reordered=0;
};

class NetworkSimulator {
public:
    NetworkSimulator();
    ~NetworkSimulator();

    NetworkSimulator(const NetworkSimulator&)=delete;
    NetworkSimulator& operator=(const NetworkSimulator&)=delete;

    NetworkSimulationEvent schedule(std::span<const std::byte> packet,const NetworkSimulationSettings& settings);
    bool popReady(std::span<std::byte> packet,std::size_t& size);
    std::size_t queuedPackets() const;
    void clear();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}
