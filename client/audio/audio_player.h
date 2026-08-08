#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

struct pa_simple;

// 多路混音播放器:按 SSRC 维护每路 20ms PCM 队列,播放线程每周期
// 从各路取一帧逐样本叠加(int32 累加防溢出,饱和裁剪回 int16)后写入。
// pa_simple_write 本身阻塞 20ms,天然充当播放时钟。
// 1v1 只有一路时行为等价于普通播放器。
class AudioPlayer {
public:
    struct Frame {
        std::vector<int16_t> pcm;  // 960 样本
        int64_t timestamp_ms = 0;  // 发送端采集时刻(音画同步统计用)
    };

    ~AudioPlayer();
    bool start();
    void stop();
    void push(uint32_t ssrc, Frame frame);
    uint64_t playedFrames() const { return played_; }
    int64_t lastPlayedTs() const { return last_ts_; }

private:
    static constexpr size_t kMaxQueuedPerSource = 10;  // 200ms 上限,满丢最旧

    void playLoop();

    pa_simple* pa_ = nullptr;
    std::mutex mutex_;
    std::map<uint32_t, std::deque<Frame>> sources_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> played_{0};
    std::atomic<int64_t> last_ts_{0};
};
