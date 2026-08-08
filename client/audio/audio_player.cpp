#include "client/audio/audio_player.h"

#include <pulse/error.h>
#include <pulse/simple.h>

#include <cstdio>

#include "client/audio/opus_codec.h"

AudioPlayer::~AudioPlayer() {
    stop();
}

bool AudioPlayer::start() {
    if (running_) return false;

    pa_sample_spec ss{};
    ss.format = PA_SAMPLE_S16LE;
    ss.rate = kAudioSampleRate;
    ss.channels = kAudioChannels;
    // tlength 限制服务端缓冲(4 帧 = 80ms),太大会增加端到端延迟
    pa_buffer_attr attr{};
    attr.maxlength = static_cast<uint32_t>(-1);
    attr.tlength = kAudioFrameBytes * 4;
    attr.prebuf = static_cast<uint32_t>(-1);
    attr.minreq = static_cast<uint32_t>(-1);

    int err = 0;
    pa_ = pa_simple_new(nullptr, "avclient", PA_STREAM_PLAYBACK, nullptr,
                        "playback", &ss, nullptr, &attr, &err);
    if (!pa_) {
        std::fprintf(stderr, "AudioPlayer: pa_simple_new: %s\n",
                     pa_strerror(err));
        return false;
    }
    played_ = 0;
    running_ = true;
    thread_ = std::thread(&AudioPlayer::playLoop, this);
    return true;
}

void AudioPlayer::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    if (pa_) {
        pa_simple_free(pa_);
        pa_ = nullptr;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    sources_.clear();
}

void AudioPlayer::push(uint32_t ssrc, Frame frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& q = sources_[ssrc];
    if (q.size() >= kMaxQueuedPerSource) q.pop_front();  // 防延迟累积
    q.push_back(std::move(frame));
}

void AudioPlayer::playLoop() {
    while (running_) {
        // 每周期从各路取一帧混音
        int32_t mix[kAudioFrameSamples] = {0};
        int active = 0;
        int64_t newest_ts = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& [ssrc, q] : sources_) {
                if (q.empty()) continue;
                const auto& f = q.front();
                for (int i = 0; i < kAudioFrameSamples; ++i) {
                    mix[i] += f.pcm[i];
                }
                if (f.timestamp_ms > newest_ts) newest_ts = f.timestamp_ms;
                q.pop_front();
                ++active;
            }
        }

        int16_t out[kAudioFrameSamples];
        for (int i = 0; i < kAudioFrameSamples; ++i) {
            // 饱和裁剪:多路叠加可能超 int16 范围
            int32_t v = mix[i];
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            out[i] = static_cast<int16_t>(v);
        }

        int err = 0;
        // 没有活跃源时写静音,pa_simple_write 的阻塞就是 20ms 节拍器
        if (pa_simple_write(pa_, out, sizeof(out), &err) < 0) {
            std::fprintf(stderr, "AudioPlayer: write: %s\n", pa_strerror(err));
            break;
        }
        if (active > 0) {
            ++played_;
            last_ts_ = newest_ts;
        }
    }
}
