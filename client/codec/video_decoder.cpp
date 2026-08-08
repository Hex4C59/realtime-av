#include "client/codec/video_decoder.h"

#include <cstdio>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
}

VideoDecoder::~VideoDecoder() {
    if (ctx_) avcodec_free_context(&ctx_);
    if (av_frame_) av_frame_free(&av_frame_);
    if (pkt_) av_packet_free(&pkt_);
}

bool VideoDecoder::init() {
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        std::fprintf(stderr, "VideoDecoder: h264 decoder not found\n");
        return false;
    }
    ctx_ = avcodec_alloc_context3(codec);
    if (avcodec_open2(ctx_, codec, nullptr) < 0) {
        std::fprintf(stderr, "VideoDecoder: avcodec_open2 failed\n");
        return false;
    }
    av_frame_ = av_frame_alloc();
    pkt_ = av_packet_alloc();
    return true;
}

std::vector<std::shared_ptr<VideoFrame>> VideoDecoder::decode(
    const uint8_t* data, size_t size, int64_t timestamp_ms) {
    std::vector<std::shared_ptr<VideoFrame>> out;

    if (av_new_packet(pkt_, static_cast<int>(size)) < 0) return out;
    std::memcpy(pkt_->data, data, size);
    pkt_->pts = timestamp_ms;

    int ret = avcodec_send_packet(ctx_, pkt_);
    av_packet_unref(pkt_);
    if (ret < 0) {
        // 单帧解码失败不致命(比如网络场景丢了参考帧),打日志继续
        std::fprintf(stderr, "VideoDecoder: send_packet failed (%d)\n", ret);
        return out;
    }

    while (avcodec_receive_frame(ctx_, av_frame_) == 0) {
        auto frame = std::make_shared<VideoFrame>(
            VideoFrame::alloc(av_frame_->width, av_frame_->height));
        frame->timestamp_ms = av_frame_->pts;

        // 解码输出同样带行对齐 padding,逐行拷成紧凑布局
        uint8_t* dst[3] = {frame->y(), frame->u(), frame->v()};
        const int dst_stride[3] = {frame->width, frame->width / 2,
                                   frame->width / 2};
        const int heights[3] = {frame->height, frame->height / 2,
                                frame->height / 2};
        for (int p = 0; p < 3; ++p) {
            for (int row = 0; row < heights[p]; ++row) {
                std::memcpy(dst[p] + row * dst_stride[p],
                            av_frame_->data[p] + row * av_frame_->linesize[p],
                            static_cast<size_t>(dst_stride[p]));
            }
        }
        out.push_back(std::move(frame));
    }
    return out;
}
