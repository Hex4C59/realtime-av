#pragma once

#include <netinet/in.h>

#include <cstddef>
#include <cstdint>
#include <string>

// 极简 UDP 封装:发送端 open(0) + setPeer + send;接收端 open(port) + recv
class UdpSocket {
public:
    ~UdpSocket();

    bool open(uint16_t bind_port);  // 0 = 系统随机分配端口(发送端用)
    uint16_t localPort() const;     // 实际绑定的本地端口(信令上报用)
    bool setPeer(const std::string& ip, uint16_t port);
    bool send(const uint8_t* data, size_t size);
    // 返回值:>0 收到的字节数;0 超时;<0 出错
    int recv(uint8_t* buf, size_t cap, int timeout_ms);
    void close();

private:
    int fd_ = -1;
    sockaddr_in peer_{};
    bool has_peer_ = false;
};
