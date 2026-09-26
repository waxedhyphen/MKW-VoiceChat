#include "mkwvc/UdpVoiceTransport.hpp"

#include <array>
#include <stdexcept>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace mkwvc {

#ifdef _WIN32
using SocketHandle=SOCKET;
static constexpr SocketHandle InvalidSocket=INVALID_SOCKET;
#else
using SocketHandle=int;
static constexpr SocketHandle InvalidSocket=-1;
#endif

namespace {

void closeSocket(SocketHandle socket) {
    if(socket==InvalidSocket) return;
#ifdef _WIN32
    closesocket(socket);
#else
    close(socket);
#endif
}

std::string discoverLocalAddress(const sockaddr_in& peer) {
    const auto routeSocket=::socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    if(routeSocket==InvalidSocket) return {};

    if(::connect(routeSocket,reinterpret_cast<const sockaddr*>(&peer),sizeof(peer))!=0) {
        closeSocket(routeSocket);
        return {};
    }

    sockaddr_in local{};
#ifdef _WIN32
    int localLength=sizeof(local);
#else
    socklen_t localLength=sizeof(local);
#endif

    if(::getsockname(routeSocket,reinterpret_cast<sockaddr*>(&local),&localLength)!=0) {
        closeSocket(routeSocket);
        return {};
    }

    std::array<char,INET_ADDRSTRLEN> address{};
    const auto result=inet_ntop(AF_INET,&local.sin_addr,address.data(),static_cast<socklen_t>(address.size()));
    closeSocket(routeSocket);
    return result ? std::string(address.data()) : std::string{};
}

}

class UdpVoiceTransport::Impl {
public:
    Impl(std::string peerAddress,std::uint16_t port) {
#ifdef _WIN32
        WSADATA data{};
        if(WSAStartup(MAKEWORD(2,2),&data)!=0) throw std::runtime_error("WSAStartup failed");
        winsockStarted_=true;
#endif
        socket_=::socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
        if(socket_==InvalidSocket) throw std::runtime_error("Failed to create UDP socket");

        sockaddr_in local{};
        local.sin_family=AF_INET;
        local.sin_addr.s_addr=htonl(INADDR_ANY);
        local.sin_port=htons(port);
        if(::bind(socket_,reinterpret_cast<const sockaddr*>(&local),sizeof(local))!=0) throw std::runtime_error("Failed to bind UDP port");

        peer_.sin_family=AF_INET;
        peer_.sin_port=htons(port);
        if(inet_pton(AF_INET,peerAddress.c_str(),&peer_.sin_addr)!=1) throw std::runtime_error("Peer address must be an IPv4 address");

        localAddress_=discoverLocalAddress(peer_);

#ifdef _WIN32
        DWORD timeoutMs=5;
        setsockopt(socket_,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&timeoutMs),sizeof(timeoutMs));
#else
        timeval timeout{};
        timeout.tv_usec=5000;
        setsockopt(socket_,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
#endif
    }

    ~Impl() {
        closeSocket(socket_);
#ifdef _WIN32
        if(winsockStarted_) WSACleanup();
#endif
    }

    bool send(std::span<const std::byte> packet) {
#ifdef _WIN32
        const auto result=::sendto(socket_,reinterpret_cast<const char*>(packet.data()),static_cast<int>(packet.size()),0,reinterpret_cast<const sockaddr*>(&peer_),sizeof(peer_));
        if(result==SOCKET_ERROR) throw std::runtime_error("UDP send failed");
#else
        const auto result=::sendto(socket_,packet.data(),packet.size(),0,reinterpret_cast<const sockaddr*>(&peer_),sizeof(peer_));
        if(result<0) throw std::runtime_error("UDP send failed");
#endif
        return true;
    }

    std::size_t receive(std::span<std::byte> packet) {
        sockaddr_in source{};
#ifdef _WIN32
        int sourceLength=sizeof(source);
        const auto result=::recvfrom(socket_,reinterpret_cast<char*>(packet.data()),static_cast<int>(packet.size()),0,reinterpret_cast<sockaddr*>(&source),&sourceLength);
        if(result==SOCKET_ERROR) return 0;
#else
        socklen_t sourceLength=sizeof(source);
        const auto result=::recvfrom(socket_,packet.data(),packet.size(),0,reinterpret_cast<sockaddr*>(&source),&sourceLength);
        if(result<0) return 0;
#endif
        if(source.sin_addr.s_addr!=peer_.sin_addr.s_addr || source.sin_port!=peer_.sin_port) return 0;
        return static_cast<std::size_t>(result);
    }

    std::string localAddress() const {
        return localAddress_;
    }

private:
    SocketHandle socket_=InvalidSocket;
    sockaddr_in peer_{};
    std::string localAddress_;
#ifdef _WIN32
    bool winsockStarted_=false;
#endif
};

UdpVoiceTransport::UdpVoiceTransport(std::string peerAddress,std::uint16_t port):impl_(std::make_unique<Impl>(std::move(peerAddress),port)) {}
UdpVoiceTransport::~UdpVoiceTransport()=default;
bool UdpVoiceTransport::send(std::span<const std::byte> packet){return impl_->send(packet);}
std::size_t UdpVoiceTransport::receive(std::span<std::byte> packet){return impl_->receive(packet);}
std::string UdpVoiceTransport::localAddress() const{return impl_->localAddress();}

}
