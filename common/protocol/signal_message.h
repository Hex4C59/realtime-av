#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "third_party/nlohmann/json.hpp"

// 信令帧协议:[4 字节大端 body 长度][JSON body]
//
// TCP 是字节流,没有"消息边界"——一次 recv 可能收到半条消息(拆包)、
// 也可能收到两条半消息(粘包)。解决办法就是自定义边界:每条消息前放
// 4 字节长度,接收方攒够 4+len 字节才算收齐一条。
//
// 消息类型(type 字段):
//   客户端→服务器: login{name} / call{to} / answer{to,accept} /
//                   media_info{to,udp_port} / hangup{to} / heartbeat
//   服务器→客户端: login_ack{ok,reason} / user_list{users[]} /
//                   incoming_call{from} / call_result{peer,accept,reason} /
//                   peer_media{peer,ip,udp_port} / peer_hangup{peer}
namespace protocol {

using nlohmann::json;

constexpr size_t kMaxBodySize = 64 * 1024;  // 防御异常长度,信令消息都很小

// 一条 JSON 消息 → 带长度前缀的完整帧
inline std::vector<uint8_t> encodeFrame(const json& msg) {
    std::string body = msg.dump();
    std::vector<uint8_t> frame(4 + body.size());
    uint32_t len = static_cast<uint32_t>(body.size());
    frame[0] = static_cast<uint8_t>(len >> 24);
    frame[1] = static_cast<uint8_t>(len >> 16);
    frame[2] = static_cast<uint8_t>(len >> 8);
    frame[3] = static_cast<uint8_t>(len);
    memcpy(frame.data() + 4, body.data(), body.size());
    return frame;
}

// 粘包/拆包处理:喂入任意长度的字节流,凑齐完整帧就产出
class FrameParser {
public:
    // 返回本次新解出的完整消息(0 或多条);协议错误时 ok 置 false
    std::vector<json> feed(const uint8_t* data, size_t size, bool* ok) {
        *ok = true;
        buf_.insert(buf_.end(), data, data + size);
        std::vector<json> out;
        while (buf_.size() >= 4) {
            uint32_t len = (static_cast<uint32_t>(buf_[0]) << 24) |
                           (static_cast<uint32_t>(buf_[1]) << 16) |
                           (static_cast<uint32_t>(buf_[2]) << 8) |
                           static_cast<uint32_t>(buf_[3]);
            if (len > kMaxBodySize) {
                *ok = false;  // 长度非法,协议已乱,调用方应断开连接
                return out;
            }
            if (buf_.size() < 4 + len) break;  // 半包:等下次数据
            json msg = json::parse(buf_.begin() + 4, buf_.begin() + 4 + len,
                                   nullptr, /*allow_exceptions=*/false);
            if (msg.is_discarded()) {
                *ok = false;
                return out;
            }
            out.push_back(std::move(msg));
            buf_.erase(buf_.begin(), buf_.begin() + 4 + len);
        }
        return out;
    }

private:
    std::vector<uint8_t> buf_;
};

}  // namespace protocol
