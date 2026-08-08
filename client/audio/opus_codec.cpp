#include "client/audio/opus_codec.h"

#include <opus/opus.h>

#include <cstdio>

AudioEncoder::~AudioEncoder() {
    if (enc_) opus_encoder_destroy(enc_);
}

bool AudioEncoder::init(int bitrate_bps) {
    int err = 0;
    // VOIP 模式:针对语音优化(语音清晰度优先于音乐保真)
    enc_ = opus_encoder_create(kAudioSampleRate, kAudioChannels,
                               OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK) {
        std::fprintf(stderr, "AudioEncoder: create failed: %s\n",
                     opus_strerror(err));
        return false;
    }
    opus_encoder_ctl(enc_, OPUS_SET_BITRATE(bitrate_bps));
    return true;
}

std::vector<uint8_t> AudioEncoder::encode(const int16_t* pcm) {
    std::vector<uint8_t> out(1500);
    opus_int32 n = opus_encode(enc_, pcm, kAudioFrameSamples, out.data(),
                               static_cast<opus_int32>(out.size()));
    if (n < 0) {
        std::fprintf(stderr, "AudioEncoder: encode failed: %s\n",
                     opus_strerror(n));
        return {};
    }
    out.resize(static_cast<size_t>(n));
    return out;
}

AudioDecoder::~AudioDecoder() {
    if (dec_) opus_decoder_destroy(dec_);
}

bool AudioDecoder::init() {
    int err = 0;
    dec_ = opus_decoder_create(kAudioSampleRate, kAudioChannels, &err);
    if (err != OPUS_OK) {
        std::fprintf(stderr, "AudioDecoder: create failed: %s\n",
                     opus_strerror(err));
        return false;
    }
    return true;
}

std::vector<int16_t> AudioDecoder::decode(const uint8_t* data, size_t size) {
    std::vector<int16_t> pcm(kAudioFrameSamples);
    int n = opus_decode(dec_, data, static_cast<opus_int32>(size), pcm.data(),
                        kAudioFrameSamples, /*decode_fec=*/0);
    if (n != kAudioFrameSamples) {
        std::fprintf(stderr, "AudioDecoder: decode returned %d\n", n);
        return {};
    }
    return pcm;
}
