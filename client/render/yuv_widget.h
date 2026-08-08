#pragma once

#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QOpenGLWidget>
#include <atomic>
#include <memory>
#include <mutex>

#include "common/util/video_frame.h"

// YUV420P 渲染部件:三个单通道纹理 + shader 做 BT.601 YUV→RGB。
// setFrame() 可以从任意线程调用,内部通过队列事件通知主线程重绘。
class YuvWidget : public QOpenGLWidget, protected QOpenGLFunctions_3_3_Core {
public:
    explicit YuvWidget(QWidget* parent = nullptr);
    ~YuvWidget() override;

    void setFrame(std::shared_ptr<VideoFrame> frame);
    uint64_t renderedFrames() const { return rendered_frames_; }

    // 调试:取当前待渲染帧(CPU 侧),用于和 GPU 抓图对照
    std::shared_ptr<VideoFrame> currentFrame() {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        return pending_frame_;
    }

protected:
    void initializeGL() override;
    void paintGL() override;

private:
    void ensureTextures(int w, int h);

    std::mutex frame_mutex_;
    std::shared_ptr<VideoFrame> pending_frame_;
    std::atomic<bool> update_pending_{false};

    QOpenGLShaderProgram program_;
    GLuint textures_[3] = {0, 0, 0};  // Y, U, V
    GLuint vao_ = 0;
    GLuint vbo_ = 0;
    int tex_width_ = 0;
    int tex_height_ = 0;
    std::atomic<uint64_t> rendered_frames_{0};
};
