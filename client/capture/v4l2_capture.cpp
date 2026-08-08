#include "client/capture/v4l2_capture.h"

#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>

extern "C" {
#include <libswscale/swscale.h>
#include <libavutil/pixfmt.h>
}

namespace {

// ioctl 可能被信号打断(EINTR),按惯例重试
int xioctl(int fd, unsigned long request, void* arg) {
    int r;
    do {
        r = ioctl(fd, request, arg);
    } while (r == -1 && errno == EINTR);
    return r;
}

int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

constexpr int kBufferCount = 4;

}  // namespace

std::vector<V4l2Capture::DeviceInfo> V4l2Capture::listDevices() {
    std::vector<DeviceInfo> devices;
    for (int i = 0; i < 32; ++i) {
        std::string path = "/dev/video" + std::to_string(i);
        int fd = open(path.c_str(), O_RDWR | O_NONBLOCK);
        if (fd < 0) continue;

        v4l2_capability cap{};
        bool usable = xioctl(fd, VIDIOC_QUERYCAP, &cap) == 0 &&
                      (cap.device_caps & V4L2_CAP_VIDEO_CAPTURE);
        if (usable) {
            // 同一物理摄像头会暴露多个节点(视频/metadata/红外),
            // 只保留能出 YUYV 的那个
            usable = false;
            v4l2_fmtdesc fmt{};
            fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            for (fmt.index = 0; xioctl(fd, VIDIOC_ENUM_FMT, &fmt) == 0; ++fmt.index) {
                if (fmt.pixelformat == V4L2_PIX_FMT_YUYV) {
                    usable = true;
                    break;
                }
            }
        }
        if (usable) {
            devices.push_back({path, reinterpret_cast<const char*>(cap.card)});
        }
        close(fd);
    }
    return devices;
}

V4l2Capture::~V4l2Capture() {
    stop();
}

bool V4l2Capture::start(const std::string& device, int width, int height, int fps,
                        FrameCallback on_frame) {
    if (running_) {
        std::fprintf(stderr, "V4l2Capture: already running\n");
        return false;
    }
    if (!initDevice(device, width, height, fps)) {
        cleanup();
        return false;
    }
    on_frame_ = std::move(on_frame);
    captured_frames_ = 0;
    running_ = true;
    thread_ = std::thread(&V4l2Capture::captureLoop, this);
    return true;
}

void V4l2Capture::stop() {
    running_ = false;
    if (thread_.joinable()) {
        thread_.join();
    }
    cleanup();
}

bool V4l2Capture::initDevice(const std::string& device, int width, int height, int fps) {
    fd_ = open(device.c_str(), O_RDWR | O_NONBLOCK);
    if (fd_ < 0) {
        std::perror("V4l2Capture: open device");
        return false;
    }

    v4l2_capability cap{};
    if (xioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0) {
        std::perror("V4l2Capture: VIDIOC_QUERYCAP");
        return false;
    }
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) ||
        !(cap.capabilities & V4L2_CAP_STREAMING)) {
        std::fprintf(stderr, "V4l2Capture: device lacks capture/streaming capability\n");
        return false;
    }

    // 请求 YUYV;驱动可能调整分辨率,以返回值为准
    v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = width;
    fmt.fmt.pix.height = height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (xioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
        std::perror("V4l2Capture: VIDIOC_S_FMT");
        return false;
    }
    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV) {
        std::fprintf(stderr, "V4l2Capture: driver refused YUYV format\n");
        return false;
    }
    width_ = static_cast<int>(fmt.fmt.pix.width);
    height_ = static_cast<int>(fmt.fmt.pix.height);

    v4l2_streamparm parm{};
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe = {1, static_cast<uint32_t>(fps)};
    xioctl(fd_, VIDIOC_S_PARM, &parm);  // 帧率设不上不算致命错误

    // mmap 队列:内核和用户态共享缓冲区,避免每帧一次拷贝
    v4l2_requestbuffers req{};
    req.count = kBufferCount;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd_, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) {
        std::perror("V4l2Capture: VIDIOC_REQBUFS");
        return false;
    }

    buffers_.resize(req.count);
    for (uint32_t i = 0; i < req.count; ++i) {
        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (xioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) {
            std::perror("V4l2Capture: VIDIOC_QUERYBUF");
            return false;
        }
        buffers_[i].length = buf.length;
        buffers_[i].start = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE,
                                 MAP_SHARED, fd_, buf.m.offset);
        if (buffers_[i].start == MAP_FAILED) {
            std::perror("V4l2Capture: mmap");
            buffers_[i].start = nullptr;
            return false;
        }
        if (xioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
            std::perror("V4l2Capture: VIDIOC_QBUF");
            return false;
        }
    }

    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
        std::perror("V4l2Capture: VIDIOC_STREAMON");
        return false;
    }

    sws_ = sws_getContext(width_, height_, AV_PIX_FMT_YUYV422,
                          width_, height_, AV_PIX_FMT_YUV420P,
                          SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws_) {
        std::fprintf(stderr, "V4l2Capture: sws_getContext failed\n");
        return false;
    }
    return true;
}

void V4l2Capture::captureLoop() {
    while (running_) {
        pollfd pfd{fd_, POLLIN, 0};
        int r = poll(&pfd, 1, 500);
        if (r <= 0) continue;  // 超时或被打断,回头检查 running_

        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (xioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN) continue;
            std::perror("V4l2Capture: VIDIOC_DQBUF");
            break;
        }

        auto frame = std::make_shared<VideoFrame>(VideoFrame::alloc(width_, height_));
        frame->timestamp_ms = nowMs();

        const uint8_t* src[1] = {static_cast<const uint8_t*>(buffers_[buf.index].start)};
        const int src_stride[1] = {width_ * 2};  // YUYV 每像素 2 字节
        uint8_t* dst[3] = {frame->y(), frame->u(), frame->v()};
        const int dst_stride[3] = {width_, width_ / 2, width_ / 2};
        sws_scale(sws_, src, src_stride, 0, height_, dst, dst_stride);

        // 用完立刻还给内核,再执行回调,避免占着缓冲区
        xioctl(fd_, VIDIOC_QBUF, &buf);

        ++captured_frames_;
        if (on_frame_) on_frame_(std::move(frame));
    }
}

void V4l2Capture::cleanup() {
    if (fd_ >= 0) {
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(fd_, VIDIOC_STREAMOFF, &type);
    }
    for (auto& b : buffers_) {
        if (b.start) munmap(b.start, b.length);
    }
    buffers_.clear();
    if (sws_) {
        sws_freeContext(sws_);
        sws_ = nullptr;
    }
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
}
