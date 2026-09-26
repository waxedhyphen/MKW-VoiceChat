#include "mkwvc/NetworkSimulator.hpp"
#include "mkwvc/VoiceFormat.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <random>
#include <stdexcept>
#include <utility>
#include <vector>

namespace mkwvc {

class NetworkSimulator::Impl {
public:
    NetworkSimulationEvent schedule(std::span<const std::byte> packet,const NetworkSimulationSettings& requested) {
        if(packet.empty() || packet.size()>VoiceFormat::MaxVoicePacketBytes) throw std::runtime_error("Network simulator received an invalid packet");

        NetworkSimulationSettings settings=requested;
        settings.latencyMs=std::clamp(settings.latencyMs,0,5000);
        settings.jitterMs=std::clamp(settings.jitterMs,0,2000);
        settings.packetLossPercent=std::clamp(settings.packetLossPercent,0.0f,100.0f);
        settings.burstLossPackets=std::clamp(settings.burstLossPackets,1,50);
        settings.duplicatePercent=std::clamp(settings.duplicatePercent,0.0f,100.0f);
        settings.reorderPercent=std::clamp(settings.reorderPercent,0.0f,100.0f);

        NetworkSimulationEvent event;

        if(!settings.enabled) {
            burstRemaining_=0;
            if(!queuePacket(packet,0)) event.dropped=1;
            return event;
        }

        if(burstRemaining_>0) {
            --burstRemaining_;
            event.dropped=1;
            return event;
        }

        if(chance(settings.packetLossPercent)) {
            burstRemaining_=settings.burstLossPackets-1;
            event.dropped=1;
            return event;
        }

        int delay=settings.latencyMs;
        if(settings.jitterMs>0) {
            std::uniform_int_distribution<int> distribution(-settings.jitterMs,settings.jitterMs);
            delay+=distribution(rng_);
        }
        delay=std::max(delay,0);

        if(chance(settings.reorderPercent)) {
            delay+=std::max<int>(static_cast<int>(VoiceFormat::FrameDurationMs*2),settings.jitterMs+static_cast<int>(VoiceFormat::FrameDurationMs));
            event.reordered=1;
        }

        if(!queuePacket(packet,delay)) {
            event.dropped=1;
            return event;
        }

        if(chance(settings.duplicatePercent) && queuePacket(packet,delay+1)) event.duplicated=1;
        return event;
    }

    bool popReady(std::span<std::byte> packet,std::size_t& size) {
        size=0;
        if(queue_.empty()) return false;

        const auto next=std::min_element(queue_.begin(),queue_.end(),[](const QueuedPacket& a,const QueuedPacket& b){
            return a.due<b.due;
        });

        if(next==queue_.end() || next->due>Clock::now()) return false;
        if(packet.size()<next->size) throw std::runtime_error("Network simulator output buffer is too small");

        std::copy_n(next->data.begin(),next->size,packet.begin());
        size=next->size;
        queue_.erase(next);
        return true;
    }

    std::size_t queuedPackets() const {
        return queue_.size();
    }

    void clear() {
        queue_.clear();
        burstRemaining_=0;
    }

private:
    using Clock=std::chrono::steady_clock;

    struct QueuedPacket {
        std::array<std::byte,VoiceFormat::MaxVoicePacketBytes> data{};
        std::size_t size=0;
        Clock::time_point due{};
    };

    bool chance(float percent) {
        if(percent<=0.0f) return false;
        if(percent>=100.0f) return true;
        std::uniform_real_distribution<float> distribution(0.0f,100.0f);
        return distribution(rng_)<percent;
    }

    bool queuePacket(std::span<const std::byte> packet,int delayMs) {
        if(queue_.size()>=MaxQueuedPackets) return false;

        QueuedPacket queued;
        std::copy(packet.begin(),packet.end(),queued.data.begin());
        queued.size=packet.size();
        queued.due=Clock::now()+std::chrono::milliseconds(delayMs);
        queue_.push_back(std::move(queued));
        return true;
    }

    static constexpr std::size_t MaxQueuedPackets=4096;
    std::vector<QueuedPacket> queue_;
    std::mt19937 rng_{std::random_device{}()};
    int burstRemaining_=0;
};

NetworkSimulator::NetworkSimulator():impl_(std::make_unique<Impl>()) {}
NetworkSimulator::~NetworkSimulator()=default;
NetworkSimulationEvent NetworkSimulator::schedule(std::span<const std::byte> packet,const NetworkSimulationSettings& settings){return impl_->schedule(packet,settings);}
bool NetworkSimulator::popReady(std::span<std::byte> packet,std::size_t& size){return impl_->popReady(packet,size);}
std::size_t NetworkSimulator::queuedPackets() const{return impl_->queuedPackets();}
void NetworkSimulator::clear(){impl_->clear();}

}
