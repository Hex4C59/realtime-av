#include "common/net/udp_socket.h"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

UdpSocket::~UdpSocket() {
    close();
}

bool UdpSocket::open(uint16_t bind_port) {
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) {
        std::perror("UdpSocket: socket");
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(bind_port);
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("UdpSocket: bind");
        close();
        return false;
    }
    return true;
}

uint16_t UdpSocket::localPort() const {
    if (fd_ < 0) return 0;
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len) < 0) return 0;
    return ntohs(addr.sin_port);
}

bool UdpSocket::setPeer(const std::string& ip, uint16_t port) {
    peer_ = {};
    peer_.sin_family = AF_INET;
    peer_.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &peer_.sin_addr) != 1) {
        std::fprintf(stderr, "UdpSocket: bad ip %s\n", ip.c_str());
        return false;
    }
    has_peer_ = true;
    return true;
}

bool UdpSocket::send(const uint8_t* data, size_t size) {
    if (fd_ < 0 || !has_peer_) return false;
    ssize_t n = ::sendto(fd_, data, size, 0,
                         reinterpret_cast<const sockaddr*>(&peer_), sizeof(peer_));
    return n == static_cast<ssize_t>(size);
}

int UdpSocket::recv(uint8_t* buf, size_t cap, int timeout_ms) {
    if (fd_ < 0) return -1;
    pollfd pfd{fd_, POLLIN, 0};
    int r = ::poll(&pfd, 1, timeout_ms);
    if (r == 0) return 0;
    if (r < 0) return -1;
    ssize_t n = ::recvfrom(fd_, buf, cap, 0, nullptr, nullptr);
    return n < 0 ? -1 : static_cast<int>(n);
}

void UdpSocket::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}
