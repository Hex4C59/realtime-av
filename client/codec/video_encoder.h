#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include "common/util/video_frame.h"

struct AVCodecContext;
struct AVFrame;
struct AVPacket;

// 一个编码后的 H.264 访问单元(一帧),Annex-B 格式:
// 内部含若干 NALU,每个前面有 00 00 00 01 起始码;
// 关键帧前带 SPS/PPS(不设 GLOBAL_HEADER 时 libx264 的默认行为),
// 这样从任意关键帧开始都能解码——网络传输必需。
struct EncodedPacket {
    std::vector<uint8_t> data;
    int64_t timestamp_ms = 0;
    bool keyframe = false;
};

class VideoEncoder {
public:
    ~VideoEncoder();

    bool init(int width, int height, int fps, int bitrate_bps);

    // 运行中改码率:x264 不支持在线重配,重开编码器实现
    // (顺带产出新 IDR,画面自刷新)。仅编码线程调用。
    bool reconfigure(int bitrate_bps);
    int currentBitrate() const { return bitrate_bps_; }

    // 编码一帧。zerolatency 配置下通常一进一出,
    // 返回空 vector 表示编码器暂未输出(不算错误)。
    std::vector<EncodedPacket> encode(const VideoFrame& frame);

    // 收到对端 PLI(画面刷新请求)时调用,下一帧强制编成 IDR。
    // 任意线程可调(网络线程收 PLI,编码线程消费)。
    void requestKeyframe() { force_keyframe_ = true; }

private:
    void freeAll();

    AVCodecContext* ctx_ = nullptr;
    AVFrame* av_frame_ = nullptr;
    AVPacket* pkt_ = nullptr;
    std::atomic<bool> force_keyframe_{false};
    int width_ = 0, height_ = 0, fps_ = 0, bitrate_bps_ = 0;
};
