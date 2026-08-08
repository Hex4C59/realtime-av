#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>

struct pa_simple;

// PulseAudio 麦克风采集:独立线程阻塞读,每凑满 20ms(960 样本)回调一次。
// pa_simple_read 是阻塞式 API,这个线程不能干别的事——这正是
// 音频要独立线程的原因。
class AudioCapture {
public:
    // 回调运行在采集线程,参数是 960 个 S16 样本 + 采集时刻(毫秒)
    using FrameCallback = std::function<void(const int16_t* pcm, int64_t ts_ms)>;

    ~AudioCapture();
    bool start(FrameCallback on_frame);
    void stop();
    uint64_t capturedFrames() const { return frames_; }

private:
    void captureLoop();

    pa_simple* pa_ = nullptr;
    FrameCallback on_frame_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> frames_{0};
};
