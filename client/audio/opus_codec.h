#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

struct OpusEncoder;
struct OpusDecoder;

// 音频参数固定:48kHz / 单声道 / 20ms 帧。
// 960 = 48000 × 0.02,正好是 Opus 的标准帧长之一,
// 采集、编码、RTP、播放全链路都按这个粒度走,无需重采样和缓冲拼接。
constexpr int kAudioSampleRate = 48000;
constexpr int kAudioChannels = 1;
constexpr int kAudioFrameSamples = 960;  // 20ms
constexpr int kAudioFrameBytes = kAudioFrameSamples * 2;  // S16LE

class AudioEncoder {
public:
    ~AudioEncoder();
    bool init(int bitrate_bps = 32000);
    // 输入 960 个 S16 样本,输出一个 Opus 包(典型 60~100 字节)
    std::vector<uint8_t> encode(const int16_t* pcm);

private:
    OpusEncoder* enc_ = nullptr;
};

class AudioDecoder {
public:
    ~AudioDecoder();
    bool init();
    // 输入一个 Opus 包,输出 960 个 S16 样本;失败返回空
    std::vector<int16_t> decode(const uint8_t* data, size_t size);

private:
    OpusDecoder* dec_ = nullptr;
};
