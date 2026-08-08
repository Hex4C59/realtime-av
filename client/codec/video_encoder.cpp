#include "client/codec/video_encoder.h"

#include <cstdio>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}

VideoEncoder::~VideoEncoder() {
    freeAll();
}

void VideoEncoder::freeAll() {
    if (ctx_) avcodec_free_context(&ctx_);
    if (av_frame_) av_frame_free(&av_frame_);
    if (pkt_) av_packet_free(&pkt_);
}

bool VideoEncoder::reconfigure(int bitrate_bps) {
    freeAll();
    return init(width_, height_, fps_, bitrate_bps);
}

bool VideoEncoder::init(int width, int height, int fps, int bitrate_bps) {
    width_ = width;
    height_ = height;
    fps_ = fps;
    bitrate_bps_ = bitrate_bps;
    const AVCodec* codec = avcodec_find_encoder_by_name("libx264");
    if (!codec) {
        std::fprintf(stderr, "VideoEncoder: libx264 not found\n");
        return false;
    }
    ctx_ = avcodec_alloc_context3(codec);
    ctx_->width = width;
    ctx_->height = height;
    ctx_->pix_fmt = AV_PIX_FMT_YUV420P;
    // 时间基用毫秒:pts 直接填 timestamp_ms,省一层换算
    ctx_->time_base = {1, 1000};
    ctx_->framerate = {fps, 1};
    ctx_->bit_rate = bitrate_bps;
    ctx_->gop_size = fps * 2;   // 每 2 秒一个关键帧
    ctx_->max_b_frames = 0;     // 实时通话禁 B 帧:B 帧要等后面的帧,必然增加延迟

    // ultrafast: 编码速度优先;zerolatency: 关掉 lookahead 等缓冲,
    // 否则 x264 会攒几十帧再出结果,平白多几百毫秒延迟
    av_opt_set(ctx_->priv_data, "preset", "ultrafast", 0);
    av_opt_set(ctx_->priv_data, "tune", "zerolatency", 0);

    int ret = avcodec_open2(ctx_, codec, nullptr);
    if (ret < 0) {
        std::fprintf(stderr, "VideoEncoder: avcodec_open2 failed (%d)\n", ret);
        return false;
    }

    av_frame_ = av_frame_alloc();
    av_frame_->format = AV_PIX_FMT_YUV420P;
    av_frame_->width = width;
    av_frame_->height = height;
    if (av_frame_get_buffer(av_frame_, 0) < 0) {
        std::fprintf(stderr, "VideoEncoder: frame buffer alloc failed\n");
        return false;
    }
    pkt_ = av_packet_alloc();
    return true;
}

std::vector<EncodedPacket> VideoEncoder::encode(const VideoFrame& frame) {
    std::vector<EncodedPacket> out;

    // 编码器内部可能还引用着上一帧的 buffer,写入前确保独占
    if (av_frame_make_writable(av_frame_) < 0) return out;

    // AVFrame 每行可能有对齐 padding(linesize >= width),必须逐行拷
    const uint8_t* src[3] = {frame.y(), frame.u(), frame.v()};
    const int src_stride[3] = {frame.width, frame.width / 2, frame.width / 2};
    const int heights[3] = {frame.height, frame.height / 2, frame.height / 2};
    for (int p = 0; p < 3; ++p) {
        for (int row = 0; row < heights[p]; ++row) {
            memcpy(av_frame_->data[p] + row * av_frame_->linesize[p],
                   src[p] + row * src_stride[p],
                   static_cast<size_t>(src_stride[p]));
        }
    }
    av_frame_->pts = frame.timestamp_ms;
    av_frame_->pict_type =
        force_keyframe_.exchange(false) ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;

    if (avcodec_send_frame(ctx_, av_frame_) < 0) return out;
    while (avcodec_receive_packet(ctx_, pkt_) == 0) {
        EncodedPacket ep;
        ep.data.assign(pkt_->data, pkt_->data + pkt_->size);
        ep.timestamp_ms = pkt_->pts;
        ep.keyframe = (pkt_->flags & AV_PKT_FLAG_KEY) != 0;
        out.push_back(std::move(ep));
        av_packet_unref(pkt_);
    }
    return out;
}
