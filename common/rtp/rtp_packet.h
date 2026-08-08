#pragma once

#include <cstddef>
#include <cstdint>

// RFC 3550 RTP 固定头(12 字节,不用 CSRC/扩展):
//
//  0                   1                   2                   3
//  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |V=2|P|X|  CC   |M|     PT      |       sequence number         |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |                           timestamp                           |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |                             SSRC                              |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//
// 多字节字段一律大端(网络字节序),手写移位避免依赖主机字节序。
struct RtpHeader {
    static constexpr size_t kSize = 12;

    bool marker = false;
    uint8_t payload_type = 96;  // 动态类型,96 = 本项目的 H.264
    uint16_t seq = 0;           // 每个 RTP 包 +1(注意:不是每帧)
    uint32_t timestamp = 0;     // 视频 90kHz;同一帧的所有分片相同
    uint32_t ssrc = 0;          // 流标识,每次会话随机生成

    void serialize(uint8_t out[kSize]) const {
        out[0] = 2 << 6;  // V=2, P=0, X=0, CC=0
        out[1] = static_cast<uint8_t>((marker ? 0x80 : 0) | (payload_type & 0x7F));
        out[2] = static_cast<uint8_t>(seq >> 8);
        out[3] = static_cast<uint8_t>(seq);
        out[4] = static_cast<uint8_t>(timestamp >> 24);
        out[5] = static_cast<uint8_t>(timestamp >> 16);
        out[6] = static_cast<uint8_t>(timestamp >> 8);
        out[7] = static_cast<uint8_t>(timestamp);
        out[8] = static_cast<uint8_t>(ssrc >> 24);
        out[9] = static_cast<uint8_t>(ssrc >> 16);
        out[10] = static_cast<uint8_t>(ssrc >> 8);
        out[11] = static_cast<uint8_t>(ssrc);
    }

    static bool parse(const uint8_t* data, size_t size, RtpHeader* out) {
        if (size < kSize) return false;
        if ((data[0] >> 6) != 2) return false;  // 版本必须是 2
        out->marker = (data[1] & 0x80) != 0;
        out->payload_type = data[1] & 0x7F;
        out->seq = static_cast<uint16_t>((data[2] << 8) | data[3]);
        out->timestamp = (static_cast<uint32_t>(data[4]) << 24) |
                         (static_cast<uint32_t>(data[5]) << 16) |
                         (static_cast<uint32_t>(data[6]) << 8) |
                         static_cast<uint32_t>(data[7]);
        out->ssrc = (static_cast<uint32_t>(data[8]) << 24) |
                    (static_cast<uint32_t>(data[9]) << 16) |
                    (static_cast<uint32_t>(data[10]) << 8) |
                    static_cast<uint32_t>(data[11]);
        return true;
    }
};
