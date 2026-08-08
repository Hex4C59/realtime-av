#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// RTP 包 → H.264 Annex-B 访问单元(RFC 6184 逆过程)。
//
// 阶段 3 简化版:假设按序到达(本机/局域网),只做:
// - Single NAL:加回 00 00 00 01 起始码,拼进当前帧
// - FU-A:S 片还原 NALU 头字节,后续片直接拼接
// - seq 出现跳变(丢包):丢弃当前残帧,等下一帧重新开始
// 乱序重排、丢包重传属于阶段 6 的 JitterBuffer,不在这里做。
class RtpDepacketizer {
public:
    struct Frame {
        std::vector<uint8_t> data;  // Annex-B,可直接喂解码器
        uint32_t timestamp90k = 0;
    };

    // 输入一个 RTP 包;当收齐一帧(marker 包到达)时返回 true 并填 out
    bool onPacket(const uint8_t* data, size_t size, Frame* out);

    uint64_t lostPackets() const { return lost_packets_; }

private:
    std::vector<uint8_t> buffer_;
    uint32_t cur_ts_ = 0;
    uint16_t expected_seq_ = 0;
    bool first_packet_ = true;
    bool dropping_ = false;  // 本帧已损坏,丢弃直到下一帧开始
    uint64_t lost_packets_ = 0;
};
