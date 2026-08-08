#include "common/rtp/rtp_depacketizer.h"

#include <cstdio>

#include "common/rtp/rtp_packet.h"

namespace {
const uint8_t kStartCode[4] = {0, 0, 0, 1};
}

bool RtpDepacketizer::onPacket(const uint8_t* data, size_t size, Frame* out) {
    RtpHeader hdr;
    if (!RtpHeader::parse(data, size, &hdr) || size <= RtpHeader::kSize) {
        return false;
    }
    const uint8_t* payload = data + RtpHeader::kSize;
    const size_t payload_size = size - RtpHeader::kSize;

    // seq 连续性检查。uint16_t 自然回绕,differ==0 即连续
    if (first_packet_) {
        first_packet_ = false;
    } else if (hdr.seq != expected_seq_) {
        uint16_t gap = static_cast<uint16_t>(hdr.seq - expected_seq_);
        lost_packets_ += gap;
        std::fprintf(stderr, "RtpDepacketizer: seq gap %u (expect %u got %u)\n",
                     gap, expected_seq_, hdr.seq);
        buffer_.clear();
        dropping_ = true;  // 当前帧不完整,丢到下一帧边界
    }
    expected_seq_ = static_cast<uint16_t>(hdr.seq + 1);

    // 时间戳变化 = 新的一帧开始,残留数据说明上一帧没等到 marker,丢弃
    if (!buffer_.empty() && hdr.timestamp != cur_ts_) {
        buffer_.clear();
        dropping_ = false;
    }
    cur_ts_ = hdr.timestamp;

    const uint8_t nal_type = payload[0] & 0x1F;
    if (nal_type >= 1 && nal_type <= 23) {
        // Single NAL:整个 payload 就是一个 NALU
        if (dropping_ && buffer_.empty()) dropping_ = false;  // 新 NALU 边界,恢复
        if (!dropping_) {
            buffer_.insert(buffer_.end(), kStartCode, kStartCode + 4);
            buffer_.insert(buffer_.end(), payload, payload + payload_size);
        }
    } else if (nal_type == 28 && payload_size > 2) {
        // FU-A:payload = FU indicator + FU header + 分片数据
        const bool start = (payload[1] & 0x80) != 0;
        if (start) {
            if (dropping_) dropping_ = false;  // S 片是安全的重新开始点
            // 还原原始 NALU 头:indicator 的 F/NRI + header 的 type
            uint8_t nal_header =
                static_cast<uint8_t>((payload[0] & 0xE0) | (payload[1] & 0x1F));
            buffer_.insert(buffer_.end(), kStartCode, kStartCode + 4);
            buffer_.push_back(nal_header);
        }
        if (!dropping_) {
            buffer_.insert(buffer_.end(), payload + 2, payload + payload_size);
        }
    } else {
        std::fprintf(stderr, "RtpDepacketizer: unsupported nal type %u\n", nal_type);
        return false;
    }

    // marker = 一帧收齐
    if (hdr.marker && !dropping_ && !buffer_.empty()) {
        out->data.swap(buffer_);
        out->timestamp90k = hdr.timestamp;
        buffer_.clear();
        return true;
    }
    if (hdr.marker) {
        // 帧尾到了但本帧已损坏:清状态,下一帧重新来
        buffer_.clear();
        dropping_ = false;
    }
    return false;
}
