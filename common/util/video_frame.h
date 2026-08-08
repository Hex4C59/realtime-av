#pragma once

#include <cstdint>
#include <vector>

// YUV420P 平面帧:data 依次连续存放 Y(w*h)、U(w*h/4)、V(w*h/4)
struct VideoFrame {
    int width = 0;
    int height = 0;
    int64_t timestamp_ms = 0;
    std::vector<uint8_t> data;

    static VideoFrame alloc(int w, int h) {
        VideoFrame f;
        f.width = w;
        f.height = h;
        f.data.resize(static_cast<size_t>(w) * h * 3 / 2);
        return f;
    }

    uint8_t* y() { return data.data(); }
    uint8_t* u() { return data.data() + ySize(); }
    uint8_t* v() { return data.data() + ySize() + uvSize(); }
    const uint8_t* y() const { return data.data(); }
    const uint8_t* u() const { return data.data() + ySize(); }
    const uint8_t* v() const { return data.data() + ySize() + uvSize(); }

    size_t ySize() const { return static_cast<size_t>(width) * height; }
    size_t uvSize() const { return ySize() / 4; }
};
