#include "common/rtp/rtp_packetizer.h"

#include <algorithm>
#include <cstring>
#include <random>

#include "common/rtp/rtp_packet.h"

namespace {

// 在 [pos, end) 中找下一个起始码(00 00 01 或 00 00 00 01),
// 返回起始码后第一个字节(NALU 首字节)的下标;找不到返回 size
size_t findNaluStart(const uint8_t* data, size_t size, size_t pos) {
    for (size_t i = pos; i + 3 <= size; ++i) {
        if (data[i] == 0 && data[i + 1] == 0) {
            if (data[i + 2] == 1) return i + 3;
            if (i + 4 <= size && data[i + 2] == 0 && data[i + 3] == 1) return i + 4;
        }
    }
    return size;
}

}  // namespace

RtpPacketizer::RtpPacketizer(uint8_t payload_type, uint32_t ssrc, size_t mtu)
    : payload_type_(payload_type),
      ssrc_(ssrc),
      max_payload_(mtu - RtpHeader::kSize),
      // 初始 seq 随机(RFC 3550 建议,降低被注入伪造包的可能)
      seq_(static_cast<uint16_t>(std::random_device{}())) {}

std::vector<std::vector<uint8_t>> RtpPacketizer::packetize(const uint8_t* data,
                                                           size_t size,
                                                           uint32_t timestamp90k) {
    std::vector<std::vector<uint8_t>> out;

    // 逐个 NALU:起始码只是 Annex-B 的分隔符,不进 RTP
    size_t nalu_begin = findNaluStart(data, size, 0);
    while (nalu_begin < size) {
        size_t next_start = findNaluStart(data, size, nalu_begin);
        // NALU 结束位置 = 下一个起始码之前(减去起始码本身的 3/4 字节)
        size_t nalu_end = next_start;
        if (next_start < size) {
            nalu_end = next_start - 3;  // 回退 00 00 01
            // 若是 4 字节起始码 00 00 00 01,再回退一个 0
            if (nalu_end > nalu_begin && data[nalu_end - 1] == 0) --nalu_end;
        }
        packNalu(data + nalu_begin, nalu_end - nalu_begin, timestamp90k, &out);
        nalu_begin = next_start;
    }

    if (!out.empty()) {
        // 本帧最后一个包置 marker,接收端据此判断一帧收齐
        out.back()[1] |= 0x80;
    }
    return out;
}

void RtpPacketizer::packNalu(const uint8_t* nalu, size_t size, uint32_t ts,
                             std::vector<std::vector<uint8_t>>* out) {
    if (size == 0) return;

    RtpHeader hdr;
    hdr.payload_type = payload_type_;
    hdr.timestamp = ts;
    hdr.ssrc = ssrc_;

    if (size <= max_payload_) {
        // Single NAL 模式:NALU 原样作 payload
        std::vector<uint8_t> pkt(RtpHeader::kSize + size);
        hdr.seq = seq_++;
        hdr.serialize(pkt.data());
        memcpy(pkt.data() + RtpHeader::kSize, nalu, size);
        out->push_back(std::move(pkt));
        return;
    }

    // FU-A 分片(RFC 6184 5.8):
    //   FU indicator = 原 NALU 头的 F/NRI 位 + type=28
    //   FU header    = S(首片)/E(尾片)标志 + 原 NALU 的 type
    // 接收端用 indicator 的前 3 位 + header 的后 5 位还原出原 NALU 头字节
    const uint8_t nal_header = nalu[0];
    const uint8_t fu_indicator = static_cast<uint8_t>((nal_header & 0xE0) | 28);
    const size_t chunk_max = max_payload_ - 2;  // 扣掉 indicator + header
    size_t offset = 1;                          // 原 NALU 头不随分片携带

    while (offset < size) {
        size_t chunk = std::min(chunk_max, size - offset);
        bool start = (offset == 1);
        bool end = (offset + chunk == size);

        std::vector<uint8_t> pkt(RtpHeader::kSize + 2 + chunk);
        hdr.seq = seq_++;
        hdr.serialize(pkt.data());
        pkt[RtpHeader::kSize] = fu_indicator;
        pkt[RtpHeader::kSize + 1] = static_cast<uint8_t>(
            (start ? 0x80 : 0) | (end ? 0x40 : 0) | (nal_header & 0x1F));
        memcpy(pkt.data() + RtpHeader::kSize + 2, nalu + offset, chunk);
        out->push_back(std::move(pkt));
        offset += chunk;
    }
}
