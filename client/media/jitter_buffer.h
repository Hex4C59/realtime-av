#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

// 视频 JitterBuffer:吸收网络乱序/抖动,输出严格按序的 RTP 包流。
//
// 核心设计:
// - 扩展序号:16 位 seq 约 90 秒回绕一次,直接比较是经典 bug。
//   用 int16_t 差值把 seq 展开成 64 位单调序号,后续全部逻辑无回绕问题。
// - 缺失跟踪:插入时发现序号跳变,把中间的 seq 记入缺失表,
//   供 NACK 调度(重发请求/重试上限)使用;迟到的包到达即"恢复"。
// - 队头超时:缺失包等了 kSkipTimeoutMs 还没来(NACK 也救不回),
//   跳过它继续播,同时由调用方发 PLI 请求关键帧刷新画面。
class JitterBuffer {
public:
    // 首次 NACK 前先等一个"乱序消解窗口":网络抖动造成的乱序 gap
    // 大多几十毫秒内自愈,立刻 NACK 会让原包+重传包都到达,白耗带宽。
    static constexpr int64_t kNackInitialDelayMs = 30;
    // 重发间隔要 ≥ RTT:重传还在路上就再请求只会翻倍带宽
    // (正规做法是动态估计 RTT,此处取保守固定值,RTT 估计留作扩展)
    static constexpr int64_t kNackIntervalMs = 100;
    static constexpr int kNackMaxTries = 4;          // 单包最多请求次数
    static constexpr int64_t kSkipTimeoutMs = 450;   // 队头等待上限(≥ 间隔×上限)

    struct Stats {
        uint64_t received = 0;
        uint64_t duplicates = 0;
        uint64_t lost_detected = 0;   // 检测到的缺口总数
        uint64_t recovered = 0;       // NACK 后等回来的包
        uint64_t skipped = 0;         // 放弃等待跳过的包
        uint64_t skip_events = 0;     // 跳帧次数(每次触发一个 PLI)
    };

    // 收到视频包(乱序/重复安全);seq 从包头解析好传入
    void insert(uint16_t seq, std::vector<uint8_t> pkt, int64_t now_ms);

    // 取下一个按序包;暂无可取返回 nullopt
    std::optional<std::vector<uint8_t>> pop();

    // 该发 NACK 的 seq 列表(受重发间隔与次数上限约束)
    std::vector<uint16_t> nacksDue(int64_t now_ms);

    // 队头缺失超时则跳过,返回 true 表示调用方应发 PLI
    bool maybeSkip(int64_t now_ms);

    size_t depth() const { return buffer_.size(); }
    const Stats& stats() const { return stats_; }

private:
    struct Missing {
        int64_t first_seen_ms = 0;  // 发现缺失的时刻
        int64_t last_nack_ms = 0;
        int tries = 0;
    };

    int64_t extend(uint16_t seq);  // 16 位 seq → 64 位扩展序号

    std::map<int64_t, std::vector<uint8_t>> buffer_;  // 扩展序号 → 包
    std::map<int64_t, Missing> missing_;
    int64_t next_ext_ = -1;     // 下一个待交付的扩展序号,-1 = 未初始化
    int64_t highest_ext_ = -1;  // 见过的最大扩展序号
    Stats stats_;
};
