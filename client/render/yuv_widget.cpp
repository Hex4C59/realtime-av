#include "client/render/yuv_widget.h"

#include <QOpenGLVertexArrayObject>
#include <cstdio>

namespace {

const char* kVertexShader = R"(
#version 330 core
layout(location = 0) in vec2 pos;
layout(location = 1) in vec2 tex;
out vec2 v_tex;
uniform vec2 scale;  // 保持画面宽高比的缩放
void main() {
    gl_Position = vec4(pos * scale, 0.0, 1.0);
    v_tex = tex;
}
)";

// BT.601 limited range(摄像头/x264 默认):Y ∈ [16,235],UV ∈ [16,240]
const char* kFragmentShader = R"(
#version 330 core
in vec2 v_tex;
out vec4 frag_color;
uniform sampler2D tex_y;
uniform sampler2D tex_u;
uniform sampler2D tex_v;
void main() {
    float y = (texture(tex_y, v_tex).r - 16.0 / 255.0) * 1.164;
    float u = texture(tex_u, v_tex).r - 0.5;
    float v = texture(tex_v, v_tex).r - 0.5;
    float r = y + 1.596 * v;
    float g = y - 0.391 * u - 0.813 * v;
    float b = y + 2.018 * u;
    frag_color = vec4(r, g, b, 1.0);
}
)";

// 全屏两个三角形:x, y, s, t
const float kQuad[] = {
    -1.0f, -1.0f, 0.0f, 1.0f,
     1.0f, -1.0f, 1.0f, 1.0f,
    -1.0f,  1.0f, 0.0f, 0.0f,
     1.0f,  1.0f, 1.0f, 0.0f,
};

}  // namespace

YuvWidget::YuvWidget(QWidget* parent) : QOpenGLWidget(parent) {}

YuvWidget::~YuvWidget() {
    makeCurrent();
    if (textures_[0]) glDeleteTextures(3, textures_);
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
    doneCurrent();
}

void YuvWidget::setFrame(std::shared_ptr<VideoFrame> frame) {
    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        pending_frame_ = std::move(frame);
    }
    // 渲染只能在主线程做,跨线程投递重绘请求。
    // 必须合并:若上一条请求还没被处理就不再投递,
    // 否则帧率高于绘制速度时事件队列被塞满,定时器等其他事件被饿死
    bool expected = false;
    if (update_pending_.compare_exchange_strong(expected, true)) {
        QMetaObject::invokeMethod(this, qOverload<>(&QWidget::update),
                                  Qt::QueuedConnection);
    }
}

void YuvWidget::initializeGL() {
    initializeOpenGLFunctions();
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);

    program_.addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexShader);
    program_.addShaderFromSourceCode(QOpenGLShader::Fragment, kFragmentShader);
    program_.link();
    program_.bind();
    program_.setUniformValue("tex_y", 0);
    program_.setUniformValue("tex_u", 1);
    program_.setUniformValue("tex_v", 2);

    glGenVertexArrays(1, &vao_);
    glBindVertexArray(vao_);
    glGenBuffers(1, &vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kQuad), kQuad, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<void*>(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
}

void YuvWidget::ensureTextures(int w, int h) {
    if (w == tex_width_ && h == tex_height_) return;
    if (textures_[0]) glDeleteTextures(3, textures_);
    glGenTextures(3, textures_);
    const int widths[3] = {w, w / 2, w / 2};
    const int heights[3] = {h, h / 2, h / 2};
    for (int i = 0; i < 3; ++i) {
        glBindTexture(GL_TEXTURE_2D, textures_[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, widths[i], heights[i], 0,
                     GL_RED, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    tex_width_ = w;
    tex_height_ = h;
}

void YuvWidget::paintGL() {
    update_pending_ = false;
    glClear(GL_COLOR_BUFFER_BIT);

    std::shared_ptr<VideoFrame> frame;
    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        frame = pending_frame_;
    }
    if (!frame) return;

    ensureTextures(frame->width, frame->height);

    // 行不一定 4 字节对齐(如 U/V 平面宽 320 之外的奇数情况),统一按 1 字节对齐上传
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    const uint8_t* planes[3] = {frame->y(), frame->u(), frame->v()};
    const int widths[3] = {frame->width, frame->width / 2, frame->width / 2};
    const int heights[3] = {frame->height, frame->height / 2, frame->height / 2};
    for (int i = 0; i < 3; ++i) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, textures_[i]);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, widths[i], heights[i],
                        GL_RED, GL_UNSIGNED_BYTE, planes[i]);
    }

    // letterbox:视频和窗口宽高比不同时留黑边而不是拉伸
    float widget_aspect = float(width()) / float(height());
    float video_aspect = float(frame->width) / float(frame->height);
    float sx = 1.0f, sy = 1.0f;
    if (widget_aspect > video_aspect) {
        sx = video_aspect / widget_aspect;
    } else {
        sy = widget_aspect / video_aspect;
    }

    program_.bind();
    program_.setUniformValue("scale", sx, sy);
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    ++rendered_frames_;
}
