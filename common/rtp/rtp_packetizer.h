#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// H.264 Annex-B 访问单元 → RTP 包(RFC 6184)。
//
// 打包前先剥掉 00 00 00 01 起始码,按 NALU 处理:
// - NALU + 12 字节 RTP 头 放得进 MTU → Single NAL 模式,NALU 原样作 payload
// - 放不下 → FU-A 分片(为什么不让 IP 层分片:IP 分片中丢任何一片,
//   整个 UDP 包都作废,重传粒度太大;FU-A 让丢包粒度停留在单个 RTP 包)
class RtpPacketizer {
public:
    RtpPacketizer(uint8_t payload_type, uint32_t ssrc, size_t mtu = 1200);

    // 输入一个完整访问单元(可含多个 NALU,如 SPS+PPS+IDR),
    // 输出若干含 RTP 头的完整包;本帧最后一个包置 marker 位
    std::vector<std::vector<uint8_t>> packetize(const uint8_t* data, size_t size,
                                                uint32_t timestamp90k);

    uint16_t nextSeq() const { return seq_; }

private:
    void packNalu(const uint8_t* nalu, size_t size, uint32_t ts,
                  std::vector<std::vector<uint8_t>>* out);

    const uint8_t payload_type_;
    const uint32_t ssrc_;
    const size_t max_payload_;  // MTU 减去 RTP 头
    uint16_t seq_;
};
