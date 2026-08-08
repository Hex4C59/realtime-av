#include "client/ui/main_window.h"

#include <QCheckBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>

#include "common/rtp/rtp_packet.h"

namespace {
struct Resolution {
    int w, h, fps;
};
// 常见 YUYV 分辨率;驱动不支持时会自动协商到最接近的
const Resolution kResolutions[] = {
    {640, 480, 30},
    {320, 240, 30},
    {848, 480, 20},
    {1280, 720, 10},
};

constexpr int kBitrateBps = 1'500'000;
constexpr uint16_t kSignalPort = 6000;
constexpr const char* kDumpPath = "loopback.h264";

int64_t steadyMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
        .count();
}
}  // namespace

MainWindow::MainWindow(bool open_camera, QWidget* parent) : QWidget(parent) {
    video_widget_ = new YuvWidget(this);
    video_widget_->setMinimumSize(320, 240);
    local_widget_ = new YuvWidget(this);
    local_widget_->setFixedSize(213, 160);
    local_widget_->hide();  // 仅通话/房间中显示本地小窗

    // 房间模式的多路宫格(默认隐藏,进房后替代大窗)
    grid_container_ = new QWidget(this);
    grid_layout_ = new QGridLayout(grid_container_);
    grid_container_->hide();

    devices_ = V4l2Capture::listDevices();
    device_box_ = new QComboBox(this);
    for (const auto& d : devices_) {
        device_box_->addItem(
            QString("%1 (%2)").arg(d.name.c_str(), d.path.c_str()));
    }

    resolution_box_ = new QComboBox(this);
    for (const auto& r : kResolutions) {
        resolution_box_->addItem(
            QString("%1x%2 @%3fps").arg(r.w).arg(r.h).arg(r.fps));
    }

    loopback_box_ = new QCheckBox("编码环回", this);

    auto* top_bar = new QHBoxLayout();
    top_bar->addWidget(new QLabel("设备:", this));
    top_bar->addWidget(device_box_, 1);
    top_bar->addWidget(new QLabel("分辨率:", this));
    top_bar->addWidget(resolution_box_);
    top_bar->addWidget(loopback_box_);
    top_bar->addStretch();

    auto* video_area = new QHBoxLayout();
    video_area->addWidget(video_widget_, 1);
    video_area->addWidget(grid_container_, 1);
    video_area->addWidget(local_widget_, 0, Qt::AlignTop);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(top_bar);
    setupSignalUi(layout);
    layout->addLayout(video_area, 1);

    connect(device_box_, &QComboBox::currentIndexChanged, this,
            [this](int) { restartCapture(); });
    connect(resolution_box_, &QComboBox::currentIndexChanged, this,
            [this](int) { restartCapture(); });
    connect(loopback_box_, &QCheckBox::toggled, this, [this](bool on) {
        if (on) {
            startLoopback();
        } else {
            stopLoopback();
        }
    });

    connect(&title_timer_, &QTimer::timeout, this, &MainWindow::updateTitle);
    title_timer_.start(1000);

    resize(960, 700);
    if (open_camera) {
        restartCapture();
    } else {
        device_box_->setEnabled(false);
        resolution_box_->setEnabled(false);
        loopback_box_->setEnabled(false);
    }
}

MainWindow::~MainWindow() {
    stopCallMedia();
    stopLoopback();
    capture_.stop();
}

uint64_t MainWindow::totalRenderedFrames() const {
    uint64_t total = video_widget_->renderedFrames();
    for (auto* w : grid_widgets_) total += w->renderedFrames();
    return total;
}

// ---------- 信令 UI:登录 / 1v1 呼叫 / 房间 ----------

void MainWindow::setupSignalUi(QVBoxLayout* layout) {
    server_edit_ = new QLineEdit("127.0.0.1", this);
    server_edit_->setFixedWidth(110);
    name_edit_ = new QLineEdit(this);
    name_edit_->setPlaceholderText("用户名");
    name_edit_->setFixedWidth(80);
    login_btn_ = new QPushButton("登录", this);
    user_box_ = new QComboBox(this);
    user_box_->setMinimumWidth(80);
    call_btn_ = new QPushButton("呼叫", this);
    answer_btn_ = new QPushButton("接听", this);
    reject_btn_ = new QPushButton("拒绝", this);
    hangup_btn_ = new QPushButton("挂断", this);
    room_edit_ = new QLineEdit(this);
    room_edit_->setPlaceholderText("房间号");
    room_edit_->setFixedWidth(70);
    join_btn_ = new QPushButton("进房", this);
    status_label_ = new QLabel("未连接", this);

    auto* bar = new QHBoxLayout();
    bar->addWidget(new QLabel("服务器:", this));
    bar->addWidget(server_edit_);
    bar->addWidget(name_edit_);
    bar->addWidget(login_btn_);
    bar->addWidget(new QLabel("在线:", this));
    bar->addWidget(user_box_);
    bar->addWidget(call_btn_);
    bar->addWidget(answer_btn_);
    bar->addWidget(reject_btn_);
    bar->addWidget(hangup_btn_);
    bar->addWidget(room_edit_);
    bar->addWidget(join_btn_);
    bar->addWidget(status_label_, 1);
    layout->addLayout(bar);

    setCallState(CallState::kIdle, "未连接");

    connect(login_btn_, &QPushButton::clicked, this, [this] {
        my_name_ = name_edit_->text().trimmed();
        if (my_name_.isEmpty()) return;
        signal_.connectToServer(server_edit_->text().trimmed(), kSignalPort);
        status_label_->setText("连接中…");
    });
    connect(call_btn_, &QPushButton::clicked, this, [this] {
        if (user_box_->currentText().isEmpty()) return;
        peer_ = user_box_->currentText();
        signal_.call(peer_);
        setCallState(CallState::kCalling, "呼叫 " + peer_ + " 中…");
    });
    connect(answer_btn_, &QPushButton::clicked, this, [this] {
        signal_.answer(peer_, true);
        setCallState(CallState::kInCall, "与 " + peer_ + " 通话中");
        startCallMedia();
    });
    connect(reject_btn_, &QPushButton::clicked, this, [this] {
        signal_.answer(peer_, false);
        setCallState(CallState::kIdle, "已就绪");
    });
    connect(hangup_btn_, &QPushButton::clicked, this, [this] {
        signal_.hangup(peer_);
        stopCallMedia();
        setCallState(CallState::kIdle, "已挂断");
    });
    connect(join_btn_, &QPushButton::clicked, this, [this] {
        if (mode_ == Mode::kRoom) {
            stopCallMedia();
            setCallState(CallState::kIdle, "已离开房间");
            join_btn_->setText("进房");
        } else if (!room_edit_->text().trimmed().isEmpty()) {
            signal_.joinRoom(room_edit_->text().trimmed());
        }
    });

    connect(&signal_, &SignalClient::connected, this,
            [this] { signal_.login(my_name_); });
    connect(&signal_, &SignalClient::disconnected, this, [this] {
        stopCallMedia();
        setCallState(CallState::kIdle, "连接断开");
    });
    connect(&signal_, &SignalClient::loginAck, this,
            [this](bool ok, const QString& reason) {
                status_label_->setText(ok ? "已登录: " + my_name_
                                          : "登录失败: " + reason);
                if (ok && !auto_join_room_.isEmpty()) {
                    room_edit_->setText(auto_join_room_);
                    signal_.joinRoom(auto_join_room_);
                }
            });
    connect(&signal_, &SignalClient::userList, this,
            [this](const QStringList& users) {
                user_box_->clear();
                for (const auto& u : users) {
                    if (u != my_name_) user_box_->addItem(u);
                }
                // 自动化:目标上线且空闲时发起呼叫(验收脚本用)
                if (!auto_call_target_.isEmpty() && !auto_call_done_ &&
                    call_state_ == CallState::kIdle &&
                    users.contains(auto_call_target_)) {
                    auto_call_done_ = true;
                    peer_ = auto_call_target_;
                    signal_.call(peer_);
                    setCallState(CallState::kCalling, "呼叫 " + peer_ + " 中…");
                }
            });
    connect(&signal_, &SignalClient::incomingCall, this,
            [this](const QString& from) {
                peer_ = from;
                setCallState(CallState::kRinging, "来电: " + from);
                if (auto_answer_) answer_btn_->click();
            });
    connect(&signal_, &SignalClient::callResult, this,
            [this](const QString& peer, bool accept, const QString& reason) {
                if (call_state_ != CallState::kCalling || peer != peer_) return;
                if (accept) {
                    setCallState(CallState::kInCall, "与 " + peer_ + " 通话中");
                    startCallMedia();
                } else {
                    setCallState(CallState::kIdle, "呼叫失败: " + reason);
                }
            });
    connect(&signal_, &SignalClient::peerMedia, this,
            [this](const QString& peer, const QString& ip, uint16_t port) {
                if (peer != peer_) return;
                std::fprintf(stderr, "peer media: %s %s:%u\n", qPrintable(peer),
                             qPrintable(ip), port);
                udp_.setPeer(ip.toStdString(), port);
                startSendPipeline();
            });
    connect(&signal_, &SignalClient::peerHangup, this, [this](const QString&) {
        stopCallMedia();
        setCallState(CallState::kIdle, "对方已挂断");
    });
    connect(&signal_, &SignalClient::sfuInfo, this,
            [this](const QString& ip, uint16_t port, const QString& room) {
                if (startRoom(ip, port, room)) {
                    join_btn_->setText("离开");
                    status_label_->setText("房间 " + room + " (SFU " + ip + ")");
                }
            });
}

void MainWindow::setCallState(CallState s, const QString& status_text) {
    call_state_ = s;
    status_label_->setText(status_text);
    bool in_room = (mode_ == Mode::kRoom);
    call_btn_->setEnabled(s == CallState::kIdle && !in_room);
    answer_btn_->setEnabled(s == CallState::kRinging);
    reject_btn_->setEnabled(s == CallState::kRinging);
    hangup_btn_->setEnabled(s == CallState::kCalling || s == CallState::kInCall);
    local_widget_->setVisible(s == CallState::kInCall || in_room);
}

void MainWindow::autoConnect(const QString& server, const QString& name,
                             const QString& call_target, bool auto_answer,
                             const QString& join_room) {
    server_edit_->setText(server);
    name_edit_->setText(name);
    auto_call_target_ = call_target;
    auto_answer_ = auto_answer;
    auto_join_room_ = join_room;
    login_btn_->click();
}

// ---------- 媒体启动/拆除 ----------

// 1v1 通话媒体:对称 RTP——收发共用一个 UDP socket,
// 本端绑随机端口后通过信令(media_info)告诉对方往这里发
void MainWindow::startCallMedia() {
    if (mode_ != Mode::kPreview) return;
    if (!udp_.open(0)) return;
    audio_player_.start();
    mode_ = Mode::kCall;
    net_running_ = true;
    net_thread_ = std::thread(&MainWindow::recvLoop, this);
    signal_.sendMediaInfo(peer_, udp_.localPort());
    std::fprintf(stderr, "call media: listening on udp %u\n", udp_.localPort());
}

// 房间媒体:媒体全部与 SFU 交互(公网下客户端主动连 SFU,天然过 NAT)
bool MainWindow::startRoom(const QString& sfu_ip, uint16_t port,
                           const QString& room) {
    if (mode_ != Mode::kPreview) return false;
    if (!udp_.open(0) || !udp_.setPeer(sfu_ip.toStdString(), port)) {
        return false;
    }
    room_name_ = room.toStdString();
    member_name_ = my_name_.toStdString();
    last_join_sent_ = 0;
    audio_player_.start();

    video_widget_->hide();
    grid_container_->show();
    local_widget_->show();

    mode_ = Mode::kRoom;
    net_running_ = true;
    net_thread_ = std::thread(&MainWindow::recvLoop, this);
    startSendPipeline();
    std::fprintf(stderr, "room media: %s via sfu %s:%u (local udp %u)\n",
                 room_name_.c_str(), qPrintable(sfu_ip), port, udp_.localPort());
    return true;
}

void MainWindow::startSendPipeline() {
    if (send_pipeline_on_) return;
    send_pipeline_on_ = true;

    // 视频流水线(有摄像头才启动;纯接收成员照样能进房收流)
    if (capture_.width() > 0) {
        encoder_ = std::make_unique<VideoEncoder>();
        const auto& r = kResolutions[resolution_box_->currentIndex()];
        if (encoder_->init(capture_.width(), capture_.height(), r.fps,
                           kBitrateBps)) {
            // 每次会话新 SSRC:防上一会话残留包污染(接收端按 SSRC 区分流)
            packetizer_ = std::make_unique<RtpPacketizer>(
                96, static_cast<uint32_t>(std::random_device{}()), 1200);
            encode_queue_.reopen();
            codec_thread_ = std::thread(&MainWindow::sendLoop, this);
        } else {
            encoder_.reset();
        }
    }

    // 音频发送:麦克风采集线程内完成 编码→RTP→发送
    // (Opus 编码一帧不到 1ms,无需再开线程)
    Mode m = mode_;
    if (m != Mode::kCall && m != Mode::kRoom) return;
    audio_encoder_ = std::make_unique<AudioEncoder>();
    if (audio_encoder_->init()) {
        audio_ssrc_ = static_cast<uint32_t>(std::random_device{}());
        audio_seq_ = static_cast<uint16_t>(std::random_device{}());
        audio_capture_.start([this](const int16_t* pcm, int64_t ts_ms) {
            auto opus = audio_encoder_->encode(pcm);
            if (opus.empty()) return;
            uint8_t pkt[1500];
            RtpHeader hdr;
            hdr.payload_type = 97;
            hdr.seq = audio_seq_++;
            hdr.timestamp = static_cast<uint32_t>(ts_ms * 48);  // 48kHz 时钟
            hdr.ssrc = audio_ssrc_;
            hdr.serialize(pkt);
            memcpy(pkt + RtpHeader::kSize, opus.data(), opus.size());
            udp_.send(pkt, RtpHeader::kSize + opus.size());
            ++audio_tx_packets_;
        });
    } else {
        audio_encoder_.reset();
    }
}

void MainWindow::stopCallMedia() {
    Mode m = mode_;
    if (m != Mode::kCall && m != Mode::kSend && m != Mode::kRecv &&
        m != Mode::kRoom) {
        return;
    }
    std::fprintf(stderr,
                 "call stats: audio tx=%llu rx=%llu played=%llu, "
                 "video pkts=%llu, jb lost=%llu recovered=%llu skips=%llu "
                 "nacks=%llu plis=%llu retrans_sent=%llu\n",
                 static_cast<unsigned long long>(audio_tx_packets_),
                 static_cast<unsigned long long>(audio_rx_packets_),
                 static_cast<unsigned long long>(audio_player_.playedFrames()),
                 static_cast<unsigned long long>(net_packets_),
                 static_cast<unsigned long long>(agg_lost_.load()),
                 static_cast<unsigned long long>(agg_recovered_.load()),
                 static_cast<unsigned long long>(agg_skips_.load()),
                 static_cast<unsigned long long>(nacks_sent_.load()),
                 static_cast<unsigned long long>(plis_sent_.load()),
                 static_cast<unsigned long long>(retrans_sent_.load()));
    mode_ = Mode::kPreview;
    audio_capture_.stop();
    audio_player_.stop();
    net_running_ = false;
    if (net_thread_.joinable()) net_thread_.join();
    if (send_pipeline_on_) {
        send_pipeline_on_ = false;
        encode_queue_.close();
        if (codec_thread_.joinable()) codec_thread_.join();
    }
    udp_.close();
    encoder_.reset();
    packetizer_.reset();
    audio_encoder_.reset();
    remote_streams_.clear();   // 接收线程已停,可安全清理
    audio_decoders_.clear();
    clearGrid();
    video_widget_->show();
    grid_container_->hide();
}

// ---------- 房间宫格(UI 线程) ----------

void MainWindow::addRemoteWidget(uint32_t ssrc) {
    auto* w = new YuvWidget(grid_container_);
    w->setMinimumSize(300, 225);
    int idx = static_cast<int>(grid_widgets_.size());
    grid_layout_->addWidget(w, idx / 2, idx % 2);
    w->show();
    grid_widgets_.push_back(w);
    std::lock_guard<std::mutex> lock(widget_mutex_);
    ready_widgets_[ssrc] = w;
}

void MainWindow::clearGrid() {
    {
        std::lock_guard<std::mutex> lock(widget_mutex_);
        ready_widgets_.clear();
    }
    for (auto* w : grid_widgets_) {
        grid_layout_->removeWidget(w);
        w->deleteLater();
    }
    grid_widgets_.clear();
}

// ---------- 采集与模式分发 ----------

bool MainWindow::selectDevice(const QString& path) {
    for (size_t i = 0; i < devices_.size(); ++i) {
        if (devices_[i].path == path.toStdString()) {
            device_box_->setCurrentIndex(static_cast<int>(i));
            return true;
        }
    }
    return false;
}

void MainWindow::setLoopback(bool on) {
    loopback_box_->setChecked(on);
}

void MainWindow::restartCapture() {
    stopLoopback();
    capture_.stop();
    if (devices_.empty()) {
        // 不能用模态弹窗:会开嵌套事件循环,卡死自检等自动化流程
        setWindowTitle("错误:未找到可用的摄像头设备");
        return;
    }
    const std::string& device = devices_[device_box_->currentIndex()].path;
    const auto& r = kResolutions[resolution_box_->currentIndex()];
    bool ok = capture_.start(
        device, r.w, r.h, r.fps, [this](std::shared_ptr<VideoFrame> frame) {
            Mode m = mode_;
            if (m == Mode::kCall || m == Mode::kRoom) {
                // 本地小窗预览 + 送编码(shared_ptr 拷贝不复制像素)
                local_widget_->setFrame(frame);
                if (send_pipeline_on_) encode_queue_.push(std::move(frame));
            } else if (m == Mode::kSend) {
                video_widget_->setFrame(frame);
                encode_queue_.push(std::move(frame));
            } else if (loopback_on_) {
                encode_queue_.push(std::move(frame));
            } else {
                video_widget_->setFrame(std::move(frame));
            }
        });
    if (!ok) {
        std::fprintf(stderr, "cannot open %s\n", device.c_str());
        setWindowTitle(QString("错误:无法打开摄像头 %1").arg(device.c_str()));
        return;
    }
    last_captured_ = 0;
    last_rendered_ = 0;
    if (loopback_box_->isChecked()) {
        startLoopback();
    }
}

// ---------- 阶段 3:无信令单向收发(测试用) ----------

bool MainWindow::startSend(const QString& ip, uint16_t port) {
    if (capture_.width() == 0) return false;
    if (!udp_.open(0) || !udp_.setPeer(ip.toStdString(), port)) return false;
    mode_ = Mode::kSend;
    loopback_box_->setEnabled(false);
    startSendPipeline();
    std::fprintf(stderr, "sending RTP to %s:%u\n", qPrintable(ip), port);
    return send_pipeline_on_;
}

bool MainWindow::startRecv(uint16_t port) {
    if (!udp_.open(port)) return false;
    mode_ = Mode::kRecv;
    net_running_ = true;
    net_thread_ = std::thread(&MainWindow::recvLoop, this);
    std::fprintf(stderr, "receiving RTP on port %u\n", port);
    return true;
}

// ---------- 媒体线程 ----------

// 发送线程:编码 → RTP 打包 → UDP 发出
void MainWindow::sendLoop() {
    while (auto frame = encode_queue_.pop()) {
        // 码率自适应:目标码率变化超 10% 时重开编码器生效
        int target = target_bitrate_.load();
        if (target > 0 && std::abs(target - encoder_->currentBitrate()) >
                              encoder_->currentBitrate() / 10) {
            std::fprintf(stderr, "bitrate adapt: %d -> %d kbps\n",
                         encoder_->currentBitrate() / 1000, target / 1000);
            encoder_->reconfigure(target);
        }
        auto packets_enc = encoder_->encode(**frame);
        ++encoded_frames_;
        for (const auto& ep : packets_enc) {
            // RTP 视频时钟 90kHz:1 毫秒 = 90 个时钟单位
            uint32_t ts90k = static_cast<uint32_t>(ep.timestamp_ms * 90);
            auto rtp_packets =
                packetizer_->packetize(ep.data.data(), ep.data.size(), ts90k);
            for (const auto& pkt : rtp_packets) {
                udp_.send(pkt.data(), pkt.size());
                // 存入历史缓存,收到对端/下游 NACK 时可重发
                uint16_t seq = static_cast<uint16_t>((pkt[2] << 8) | pkt[3]);
                send_history_.push(seq, pkt);
                net_bytes_ += pkt.size();
                ++net_packets_;
            }
            encoded_bytes_ += ep.data.size();
        }
    }
}

// 按 SSRC 建立/获取一路远端视频流水线(仅接收线程调用)
MainWindow::RemoteStream& MainWindow::ensureStream(uint32_t ssrc) {
    auto it = remote_streams_.find(ssrc);
    if (it != remote_streams_.end()) return it->second;

    RemoteStream& s = remote_streams_[ssrc];
    s.jitter = std::make_unique<JitterBuffer>();
    s.depack = std::make_unique<RtpDepacketizer>();
    s.decoder = std::make_unique<VideoDecoder>();
    s.decoder->init();
    if (mode_ == Mode::kRoom) {
        // 宫格小窗必须在 UI 线程创建,建好后经 ready_widgets_ 发布
        s.widget_requested = true;
        QMetaObject::invokeMethod(
            this, [this, ssrc] { addRemoteWidget(ssrc); },
            Qt::QueuedConnection);
    } else {
        s.widget = video_widget_;  // 1v1/单收:直接用大窗
    }
    std::fprintf(stderr, "new video stream ssrc=%08x\n", ssrc);
    return s;
}

void MainWindow::deliverStream(RemoteStream& s,
                               const std::vector<uint8_t>& pkt) {
    if (!s.depack->onPacket(pkt.data(), pkt.size(), &s.frame)) return;
    auto decoded = s.decoder->decode(s.frame.data.data(), s.frame.data.size(),
                                     s.frame.timestamp90k / 90);
    YuvWidget* w = s.widget.load();
    for (auto& d : decoded) {
        last_video_ts_ = d->timestamp_ms;
        if (w) w->setFrame(std::move(d));
    }
}

// 接收线程:UDP 收包 → 按 PT/SSRC 分流 → JitterBuffer → 解码 → 渲染
void MainWindow::recvLoop() {
    uint8_t buf[2048];
    while (net_running_) {
        int n = udp_.recv(buf, sizeof(buf), 20);
        int64_t now = steadyMs();
        Mode m = mode_;

        // 房间模式:UDP JOIN 兼作入会注册与保活(SFU 以源地址识别成员)
        if (m == Mode::kRoom && now - last_join_sent_ > 1000) {
            auto join = rtpctl::buildJoin(room_name_, member_name_);
            udp_.send(join.data(), join.size());
            last_join_sent_ = now;
        }

        if (n > 0) {
            net_bytes_ += static_cast<uint64_t>(n);
            ++net_packets_;

            RtpHeader hdr;
            if (!RtpHeader::parse(buf, static_cast<size_t>(n), &hdr)) continue;

            if (hdr.payload_type == rtpctl::kPayloadType) {
                // 控制包:NACK(重传)/ PLI(强制关键帧)/ RR(码率反馈)
                std::vector<uint16_t> seqs;
                uint8_t loss_percent = 0;
                uint8_t type = rtpctl::parse(buf, static_cast<size_t>(n), &seqs,
                                             &loss_percent);
                if (type == rtpctl::kTypeNack) {
                    for (uint16_t s : seqs) {
                        if (auto p = send_history_.get(s)) {
                            udp_.send(p->data(), p->size());
                            ++retrans_sent_;
                        }
                    }
                } else if (type == rtpctl::kTypePli && encoder_) {
                    encoder_->requestKeyframe();
                } else if (type == rtpctl::kTypeReceiverReport && encoder_) {
                    // 丢包率控制器(简版 GCC 思想:丢包驱动的 AIMD):
                    // 未恢复丢包 >10% 乘性降码率,<2% 加性缓慢回升
                    int cur = target_bitrate_.load();
                    if (cur == 0) cur = encoder_->currentBitrate();
                    int next = cur;
                    if (loss_percent > 10) {
                        next = std::max(200'000, cur * 8 / 10);
                    } else if (loss_percent < 2) {
                        next = std::min(kBitrateBps, cur + 50'000);
                    }
                    if (next != cur) target_bitrate_ = next;
                }
                continue;
            }

            if (hdr.payload_type == 97) {
                // 音频:按 SSRC 各自解码,混音播放器叠加
                auto& dec = audio_decoders_[hdr.ssrc];
                if (!dec) {
                    dec = std::make_unique<AudioDecoder>();
                    if (!dec->init()) {
                        audio_decoders_.erase(hdr.ssrc);
                        continue;
                    }
                }
                auto pcm = dec->decode(buf + RtpHeader::kSize,
                                       static_cast<size_t>(n) - RtpHeader::kSize);
                if (!pcm.empty()) {
                    audio_player_.push(hdr.ssrc,
                                       {std::move(pcm), hdr.timestamp / 48});
                    ++audio_rx_packets_;
                }
                continue;
            }

            if (hdr.payload_type == 96) {
                ensureStream(hdr.ssrc)
                    .jitter->insert(hdr.seq,
                                    std::vector<uint8_t>(buf, buf + n), now);
            }
            continue;  // JOIN ack 等其他类型忽略
        }

        // 无论有没有新包,都推进各路的交付/重传/跳帧
        uint64_t lost = 0, recovered = 0, depth = 0, skips = 0;
        for (auto& [ssrc, s] : remote_streams_) {
            // 尚未拿到 UI 线程建好的宫格窗则先领取
            if (!s.widget.load() && s.widget_requested) {
                std::lock_guard<std::mutex> lock(widget_mutex_);
                auto it = ready_widgets_.find(ssrc);
                if (it != ready_widgets_.end()) s.widget = it->second;
            }
            while (auto pkt = s.jitter->pop()) {
                deliverStream(s, *pkt);
            }
            if (nack_enabled_ && (m == Mode::kCall || m == Mode::kRoom)) {
                auto due = s.jitter->nacksDue(now);
                if (!due.empty()) {
                    auto nack = rtpctl::buildNack(ssrc, due);
                    udp_.send(nack.data(), nack.size());
                    nacks_sent_ += due.size();
                }
            }
            if (s.jitter->maybeSkip(now)) {
                if (m == Mode::kCall || m == Mode::kRoom) {
                    auto pli = rtpctl::buildPli(ssrc);
                    udp_.send(pli.data(), pli.size());
                    ++plis_sent_;
                }
                while (auto pkt = s.jitter->pop()) {
                    deliverStream(s, *pkt);  // 跳帧后继续交付后面的包
                }
            }
            const auto& st = s.jitter->stats();
            // 每秒给发送端回报未恢复丢包率(RR),驱动其码率自适应
            if ((m == Mode::kCall || m == Mode::kRoom) &&
                now - s.last_rr_ms >= 1000) {
                if (s.last_rr_ms > 0) {
                    uint64_t recv_d = st.received - s.rr_received;
                    uint64_t lost_d = st.lost_detected - s.rr_lost;
                    uint64_t rec_d = st.recovered - s.rr_recovered;
                    uint64_t unrecovered = lost_d > rec_d ? lost_d - rec_d : 0;
                    uint8_t pct = 0;
                    if (recv_d + unrecovered > 0) {
                        pct = static_cast<uint8_t>(
                            100 * unrecovered / (recv_d + unrecovered));
                    }
                    auto rr = rtpctl::buildReceiverReport(ssrc, pct);
                    udp_.send(rr.data(), rr.size());
                }
                s.last_rr_ms = now;
                s.rr_received = st.received;
                s.rr_lost = st.lost_detected;
                s.rr_recovered = st.recovered;
            }
            lost += st.lost_detected;
            recovered += st.recovered;
            skips += st.skip_events;
            depth += s.jitter->depth();
        }
        agg_lost_ = lost;
        agg_recovered_ = recovered;
        agg_skips_ = skips;
        agg_depth_ = depth;
    }
}

// ---------- 阶段 2:本地环回 ----------

void MainWindow::startLoopback() {
    if (loopback_on_ || capture_.width() == 0) return;

    encoder_ = std::make_unique<VideoEncoder>();
    decoder_ = std::make_unique<VideoDecoder>();
    const auto& r = kResolutions[resolution_box_->currentIndex()];
    if (!encoder_->init(capture_.width(), capture_.height(), r.fps, kBitrateBps) ||
        !decoder_->init()) {
        setWindowTitle("错误:编解码器初始化失败");
        encoder_.reset();
        decoder_.reset();
        return;
    }
    dump_file_.open(kDumpPath, std::ios::binary | std::ios::trunc);
    std::fprintf(stderr, "loopback on, dumping to %s\n", kDumpPath);

    encoded_bytes_ = 0;
    last_encoded_bytes_ = 0;
    encode_queue_.reopen();
    loopback_on_ = true;
    codec_thread_ = std::thread(&MainWindow::codecLoop, this);
}

void MainWindow::stopLoopback() {
    if (!loopback_on_) return;
    loopback_on_ = false;
    encode_queue_.close();  // 唤醒 codecLoop 的 pop,使线程退出
    if (codec_thread_.joinable()) codec_thread_.join();
    encoder_.reset();
    decoder_.reset();
    if (dump_file_.is_open()) dump_file_.close();
}

// 编解码线程:环回模式下的核心流水线
void MainWindow::codecLoop() {
    while (auto frame = encode_queue_.pop()) {
        auto packets = encoder_->encode(**frame);
        ++encoded_frames_;
        for (const auto& pkt : packets) {
            dump_file_.write(reinterpret_cast<const char*>(pkt.data.data()),
                             static_cast<std::streamsize>(pkt.data.size()));
            encoded_bytes_ += pkt.data.size();
            auto decoded = decoder_->decode(pkt.data.data(), pkt.data.size(),
                                            pkt.timestamp_ms);
            for (auto& d : decoded) {
                video_widget_->setFrame(std::move(d));
            }
        }
    }
}

void MainWindow::updateTitle() {
    uint64_t captured = capture_.capturedFrames();
    uint64_t rendered = totalRenderedFrames();
    QString mode_name = "预览";
    Mode m = mode_;
    if (m == Mode::kRoom) {
        mode_name = QString("房间(%1路)").arg(grid_widgets_.size());
    } else if (m == Mode::kCall) {
        mode_name = "通话";
    } else if (m == Mode::kSend) {
        mode_name = "RTP 发送";
    } else if (m == Mode::kRecv) {
        mode_name = "RTP 接收";
    } else if (loopback_on_) {
        mode_name = "编码环回";
    }
    QString title = QString("%1 — %2x%3  采集 %4 fps / 渲染 %5 fps")
                        .arg(mode_name)
                        .arg(capture_.width())
                        .arg(capture_.height())
                        .arg(captured - last_captured_)
                        .arg(rendered - last_rendered_);
    if (m != Mode::kPreview || loopback_on_) {
        uint64_t bytes = (m == Mode::kRecv) ? net_bytes_.load() : encoded_bytes_.load();
        uint64_t last = (m == Mode::kRecv) ? last_net_bytes_ : last_encoded_bytes_;
        title += QString("  码率 %1 kbps  网络包 %2")
                     .arg((bytes - last) * 8 / 1000)
                     .arg(net_packets_.load());
        title += QString("  丢包 %1 恢复 %2 NACK %3 深度 %4")
                     .arg(agg_lost_.load())
                     .arg(agg_recovered_.load())
                     .arg(nacks_sent_.load())
                     .arg(agg_depth_.load());
        if (m == Mode::kCall || m == Mode::kRoom) {
            title += QString("  音频收/发 %1/%2")
                         .arg(audio_rx_packets_.load())
                         .arg(audio_tx_packets_.load());
            int64_t a_ts = audio_player_.lastPlayedTs();
            int64_t v_ts = last_video_ts_;
            if (a_ts > 0 && v_ts > 0) {
                // 音画偏差:两路时间戳同源(发送端同一毫秒时钟),可直接相减
                title += QString("  音画差 %1ms").arg(v_ts - a_ts);
            }
        }
        if (m == Mode::kRecv) {
            last_net_bytes_ = bytes;
        } else {
            last_encoded_bytes_ = bytes;
        }
    }
    setWindowTitle(title);
    last_captured_ = captured;
    last_rendered_ = rendered;
}
