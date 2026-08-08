#pragma once

#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "common/rtp/rtp_packet.h"

// 自定义控制包(复用媒体 UDP 通道,payload type 区分):
//   NACK: [RTP头(ssrc=目标流)][1][u16 count][u16 seq]*count  请求重传
//   PLI : [RTP头(ssrc=目标流)][2]                            请求关键帧
//   JOIN: [RTP头 pt=126][1][u8 len][room][u8 len][name]      SFU 入会/保活
// 多人场景下 NACK/PLI 必须带目标 SSRC:SFU/客户端要知道请求的是哪路流。
// (正规做法是 RTCP,这里为教学简化成同通道自定义包,思想一致)
namespace rtpctl {

constexpr uint8_t kPayloadType = 127;
constexpr uint8_t kJoinPayloadType = 126;
constexpr uint8_t kTypeNack = 1;
constexpr uint8_t kTypePli = 2;
constexpr uint8_t kTypeReceiverReport = 3;  // RR: [3][u8 未恢复丢包百分比]
constexpr uint8_t kTypeJoin = 1;

inline std::vector<uint8_t> buildNack(uint32_t media_ssrc,
                                      const std::vector<uint16_t>& seqs) {
    RtpHeader hdr;
    hdr.payload_type = kPayloadType;
    hdr.ssrc = media_ssrc;
    std::vector<uint8_t> pkt(RtpHeader::kSize + 3 + seqs.size() * 2);
    hdr.serialize(pkt.data());
    uint8_t* p = pkt.data() + RtpHeader::kSize;
    *p++ = kTypeNack;
    *p++ = static_cast<uint8_t>(seqs.size() >> 8);
    *p++ = static_cast<uint8_t>(seqs.size());
    for (uint16_t s : seqs) {
        *p++ = static_cast<uint8_t>(s >> 8);
        *p++ = static_cast<uint8_t>(s);
    }
    return pkt;
}

inline std::vector<uint8_t> buildPli(uint32_t media_ssrc) {
    RtpHeader hdr;
    hdr.payload_type = kPayloadType;
    hdr.ssrc = media_ssrc;
    std::vector<uint8_t> pkt(RtpHeader::kSize + 1);
    hdr.serialize(pkt.data());
    pkt[RtpHeader::kSize] = kTypePli;
    return pkt;
}

inline std::vector<uint8_t> buildJoin(const std::string& room,
                                      const std::string& name) {
    RtpHeader hdr;
    hdr.payload_type = kJoinPayloadType;
    std::vector<uint8_t> pkt(RtpHeader::kSize + 3 + room.size() + name.size());
    hdr.serialize(pkt.data());
    uint8_t* p = pkt.data() + RtpHeader::kSize;
    *p++ = kTypeJoin;
    *p++ = static_cast<uint8_t>(room.size());
    memcpy(p, room.data(), room.size());
    p += room.size();
    *p++ = static_cast<uint8_t>(name.size());
    memcpy(p, name.data(), name.size());
    return pkt;
}

inline bool parseJoin(const uint8_t* data, size_t size, std::string* room,
                      std::string* name) {
    if (size < RtpHeader::kSize + 3) return false;
    const uint8_t* p = data + RtpHeader::kSize;
    const uint8_t* end = data + size;
    if (*p++ != kTypeJoin) return false;
    size_t rlen = *p++;
    if (p + rlen + 1 > end) return false;
    room->assign(reinterpret_cast<const char*>(p), rlen);
    p += rlen;
    size_t nlen = *p++;
    if (p + nlen > end) return false;
    name->assign(reinterpret_cast<const char*>(p), nlen);
    return true;
}

// 接收端报告:通知发送端最近一秒的未恢复丢包率,驱动码率自适应
inline std::vector<uint8_t> buildReceiverReport(uint32_t media_ssrc,
                                                uint8_t loss_percent) {
    RtpHeader hdr;
    hdr.payload_type = kPayloadType;
    hdr.ssrc = media_ssrc;
    std::vector<uint8_t> pkt(RtpHeader::kSize + 2);
    hdr.serialize(pkt.data());
    pkt[RtpHeader::kSize] = kTypeReceiverReport;
    pkt[RtpHeader::kSize + 1] = loss_percent;
    return pkt;
}

// 解析控制包 payload;返回类型字节(0 = 非法),
// NACK 填 seqs,RR 填 loss_percent
inline uint8_t parse(const uint8_t* data, size_t size,
                     std::vector<uint16_t>* seqs,
                     uint8_t* loss_percent = nullptr) {
    if (size <= RtpHeader::kSize) return 0;
    const uint8_t* p = data + RtpHeader::kSize;
    size_t n = size - RtpHeader::kSize;
    if (p[0] == kTypePli) return kTypePli;
    if (p[0] == kTypeReceiverReport && n >= 2) {
        if (loss_percent) *loss_percent = p[1];
        return kTypeReceiverReport;
    }
    if (p[0] == kTypeNack && n >= 3) {
        size_t count = (static_cast<size_t>(p[1]) << 8) | p[2];
        if (n < 3 + count * 2) return 0;
        seqs->clear();
        for (size_t i = 0; i < count; ++i) {
            seqs->push_back(
                static_cast<uint16_t>((p[3 + i * 2] << 8) | p[4 + i * 2]));
        }
        return kTypeNack;
    }
    return 0;
}

// 发送端历史包缓存:按 seq 低位做环形索引,收到 NACK 时查出重发。
// 发送线程写、接收线程读,加锁保护。
class RtpHistory {
public:
    static constexpr size_t kSlots = 1024;

    void push(uint16_t seq, const std::vector<uint8_t>& pkt) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& slot = slots_[seq % kSlots];
        slot.seq = seq;
        slot.valid = true;
        slot.data = pkt;
    }

    std::optional<std::vector<uint8_t>> get(uint16_t seq) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto& slot = slots_[seq % kSlots];
        // seq 必须精确匹配:环形槽位可能已被 1024 之后的新包覆盖
        if (!slot.valid || slot.seq != seq) return std::nullopt;
        return slot.data;
    }

private:
    struct Slot {
        bool valid = false;
        uint16_t seq = 0;
        std::vector<uint8_t> data;
    };
    std::mutex mutex_;
    Slot slots_[kSlots];
};

}  // namespace rtpctl
