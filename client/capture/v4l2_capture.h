#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "common/util/video_frame.h"

struct SwsContext;

// V4L2 摄像头采集(YUYV → YUV420P)。
// start() 后在内部线程里循环取帧,每帧转成 YUV420P 后回调 on_frame(在采集线程上执行)。
class V4l2Capture {
public:
    using FrameCallback = std::function<void(std::shared_ptr<VideoFrame>)>;

    struct DeviceInfo {
        std::string path;  // /dev/videoN
        std::string name;  // 设备名,如 "Logitech BRIO"
    };
    // 枚举支持 YUYV 视频采集的设备(跳过 metadata 等非采集节点)
    static std::vector<DeviceInfo> listDevices();

    ~V4l2Capture();

    bool start(const std::string& device, int width, int height, int fps,
               FrameCallback on_frame);
    void stop();

    // 驱动实际协商出的尺寸(可能与请求值不同)
    int width() const { return width_; }
    int height() const { return height_; }
    uint64_t capturedFrames() const { return captured_frames_; }

private:
    struct MappedBuffer {
        void* start = nullptr;
        size_t length = 0;
    };

    bool initDevice(const std::string& device, int width, int height, int fps);
    void captureLoop();
    void cleanup();

    int fd_ = -1;
    int width_ = 0;
    int height_ = 0;
    std::vector<MappedBuffer> buffers_;
    SwsContext* sws_ = nullptr;
    FrameCallback on_frame_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> captured_frames_{0};
};
