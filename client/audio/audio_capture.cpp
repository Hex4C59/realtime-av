#include "client/audio/audio_capture.h"

#include <pulse/error.h>
#include <pulse/simple.h>

#include <chrono>
#include <cstdio>

#include "client/audio/opus_codec.h"

namespace {
int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
}  // namespace

AudioCapture::~AudioCapture() {
    stop();
}

bool AudioCapture::start(FrameCallback on_frame) {
    if (running_) return false;

    pa_sample_spec ss{};
    ss.format = PA_SAMPLE_S16LE;
    ss.rate = kAudioSampleRate;
    ss.channels = kAudioChannels;
    // fragsize 设成一帧大小,让 pa 每 20ms 唤醒我们一次,降低采集延迟
    pa_buffer_attr attr{};
    attr.maxlength = static_cast<uint32_t>(-1);
    attr.fragsize = kAudioFrameBytes;

    int err = 0;
    pa_ = pa_simple_new(nullptr, "avclient", PA_STREAM_RECORD, nullptr,
                        "capture", &ss, nullptr, &attr, &err);
    if (!pa_) {
        std::fprintf(stderr, "AudioCapture: pa_simple_new: %s\n",
                     pa_strerror(err));
        return false;
    }
    on_frame_ = std::move(on_frame);
    frames_ = 0;
    running_ = true;
    thread_ = std::thread(&AudioCapture::captureLoop, this);
    return true;
}

void AudioCapture::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    if (pa_) {
        pa_simple_free(pa_);
        pa_ = nullptr;
    }
}

void AudioCapture::captureLoop() {
    int16_t buf[kAudioFrameSamples];
    while (running_) {
        int err = 0;
        if (pa_simple_read(pa_, buf, sizeof(buf), &err) < 0) {
            std::fprintf(stderr, "AudioCapture: read: %s\n", pa_strerror(err));
            break;
        }
        ++frames_;
        if (on_frame_) on_frame_(buf, nowMs());
    }
}
