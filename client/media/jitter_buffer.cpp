#include "client/media/jitter_buffer.h"

int64_t JitterBuffer::extend(uint16_t seq) {
    if (highest_ext_ < 0) return seq;  // 第一个包
    // int16_t 差值天然处理回绕:65535→0 的差是 +1 而不是 -65535
    int16_t diff = static_cast<int16_t>(
        seq - static_cast<uint16_t>(highest_ext_ & 0xFFFF));
    return highest_ext_ + diff;
}

void JitterBuffer::insert(uint16_t seq, std::vector<uint8_t> pkt,
                          int64_t now_ms) {
    int64_t ext = extend(seq);
    ++stats_.received;

    if (next_ext_ < 0) {
        // 首包:从这里开始交付
        next_ext_ = ext;
        highest_ext_ = ext;
        buffer_.emplace(ext, std::move(pkt));
        return;
    }

    if (ext < next_ext_ || buffer_.count(ext)) {
        ++stats_.duplicates;  // 已交付过或缓冲里已有(重传重复到达)
        return;
    }

    // 新的最大序号:中间未到的都记为缺失,等 NACK 调度
    if (ext > highest_ext_) {
        for (int64_t s = highest_ext_ + 1; s < ext; ++s) {
            missing_[s] = {now_ms, 0, 0};
            ++stats_.lost_detected;
        }
        highest_ext_ = ext;
    }

    // 迟到补洞(可能是重传回来的)
    auto it = missing_.find(ext);
    if (it != missing_.end()) {
        if (it->second.tries > 0) ++stats_.recovered;
        missing_.erase(it);
    }
    buffer_.emplace(ext, std::move(pkt));
}

std::optional<std::vector<uint8_t>> JitterBuffer::pop() {
    auto it = buffer_.find(next_ext_);
    if (it == buffer_.end()) return std::nullopt;
    auto pkt = std::move(it->second);
    buffer_.erase(it);
    ++next_ext_;
    return pkt;
}

std::vector<uint16_t> JitterBuffer::nacksDue(int64_t now_ms) {
    std::vector<uint16_t> due;
    for (auto& [ext, m] : missing_) {
        if (m.tries >= kNackMaxTries) continue;  // 放弃,等超时跳过
        if (m.tries == 0) {
            if (now_ms - m.first_seen_ms < kNackInitialDelayMs) continue;
        } else if (now_ms - m.last_nack_ms < kNackIntervalMs) {
            continue;
        }
        m.last_nack_ms = now_ms;
        ++m.tries;
        due.push_back(static_cast<uint16_t>(ext & 0xFFFF));
        if (due.size() >= 32) break;  // 单条 NACK 消息别太大
    }
    return due;
}

bool JitterBuffer::maybeSkip(int64_t now_ms) {
    if (buffer_.empty()) return false;
    auto head = missing_.find(next_ext_);
    if (head == missing_.end()) return false;  // 队头不缺,只是还没 pop
    if (now_ms - head->second.first_seen_ms < kSkipTimeoutMs) return false;

    // 放弃队头缺口:跳到缓冲里现存的第一个包
    int64_t first_present = buffer_.begin()->first;
    for (int64_t s = next_ext_; s < first_present; ++s) {
        auto it = missing_.find(s);
        if (it != missing_.end()) {
            missing_.erase(it);
            ++stats_.skipped;
        }
    }
    next_ext_ = first_present;
    ++stats_.skip_events;
    return true;  // 跳过后画面必然缺参考帧,调用方发 PLI 要关键帧
}
