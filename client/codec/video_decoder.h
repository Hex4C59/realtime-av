#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "common/util/video_frame.h"

struct AVCodecContext;
struct AVFrame;
struct AVPacket;

class VideoDecoder {
public:
    ~VideoDecoder();

    bool init();

    // 输入一个完整的 Annex-B 访问单元(VideoEncoder 的输出,
    // 或将来网络接收组装出的一帧),返回解出的帧(可能 0 或多帧)
    std::vector<std::shared_ptr<VideoFrame>> decode(const uint8_t* data,
                                                    size_t size,
                                                    int64_t timestamp_ms);

private:
    AVCodecContext* ctx_ = nullptr;
    AVFrame* av_frame_ = nullptr;
    AVPacket* pkt_ = nullptr;
};
