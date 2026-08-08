#pragma once

#include <QComboBox>
#include <QTimer>
#include <QWidget>
#include <atomic>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "client/audio/audio_capture.h"
#include "client/audio/audio_player.h"
#include "client/audio/opus_codec.h"
#include "client/capture/v4l2_capture.h"
#include "client/codec/video_decoder.h"
#include "client/codec/video_encoder.h"
#include "client/media/jitter_buffer.h"
#include "client/render/yuv_widget.h"
#include "client/signal/signal_client.h"
#include "common/net/udp_socket.h"
#include "common/rtp/rtp_control.h"
#include "common/rtp/rtp_depacketizer.h"
#include "common/rtp/rtp_packetizer.h"
#include "common/util/blocking_queue.h"

class QCheckBox;
class QGridLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QVBoxLayout;

class MainWindow : public QWidget {
    Q_OBJECT

public:
    // open_camera=false 用于纯接收模式(不占用摄像头)
    explicit MainWindow(bool open_camera = true, QWidget* parent = nullptr);
    ~MainWindow() override;

    // 按路径预选设备(如 /dev/video2),找不到返回 false
    bool selectDevice(const QString& path);
    void setLoopback(bool on);  // 供命令行 --loopback 使用
    void setNackEnabled(bool on) { nack_enabled_ = on; }  // --no-nack 对比演示
    // 阶段 3:单向 RTP/UDP 传输(无信令,测试用)
    bool startSend(const QString& ip, uint16_t port);
    bool startRecv(uint16_t port);
    // 阶段 4/7:命令行自动化(登录/自动呼叫/自动接听/自动进房)
    void autoConnect(const QString& server, const QString& name,
                     const QString& call_target, bool auto_answer,
                     const QString& join_room);
    YuvWidget* videoWidget() { return video_widget_; }
    // 自检用:主窗 + 宫格所有远端窗的累计渲染帧数
    uint64_t totalRenderedFrames() const;

private:
    enum class Mode { kPreview, kSend, kRecv, kCall, kRoom };
    // 呼叫状态机:Idle --呼出--> Calling --对端接受--> InCall
    //             Idle --来电--> Ringing --本端接听--> InCall
    enum class CallState { kIdle, kCalling, kRinging, kInCall };

    // 每一路远端视频流(按 SSRC 区分)的独立接收流水线。
    // 仅接收线程访问;widget 由 UI 线程创建后经原子指针发布。
    struct RemoteStream {
        std::unique_ptr<JitterBuffer> jitter;
        std::unique_ptr<RtpDepacketizer> depack;
        std::unique_ptr<VideoDecoder> decoder;
        std::atomic<YuvWidget*> widget{nullptr};
        RtpDepacketizer::Frame frame;
        bool widget_requested = false;
        // 码率自适应:上次 RR 的统计快照(算增量丢包率)
        int64_t last_rr_ms = 0;
        uint64_t rr_lost = 0, rr_recovered = 0, rr_received = 0;
    };

    void restartCapture();
    void updateTitle();
    void startLoopback();
    void stopLoopback();
    void codecLoop();
    void sendLoop();
    void recvLoop();
    RemoteStream& ensureStream(uint32_t ssrc);
    void deliverStream(RemoteStream& s, const std::vector<uint8_t>& pkt);

    // 阶段 4:1v1 呼叫;阶段 7:SFU 房间
    void setupSignalUi(QVBoxLayout* layout);
    void setCallState(CallState s, const QString& status_text);
    void startCallMedia();     // 绑 UDP + 起接收线程 + 上报 media_info
    void startSendPipeline();  // 视频(有摄像头时)+ 音频发送流水线
    void stopCallMedia();      // 1v1 与房间共用的媒体拆除
    bool startRoom(const QString& sfu_ip, uint16_t port, const QString& room);
    void addRemoteWidget(uint32_t ssrc);  // UI 线程:宫格加一路小窗
    void clearGrid();                     // UI 线程

    YuvWidget* video_widget_ = nullptr;   // 大窗:预览/1v1 远端画面
    YuvWidget* local_widget_ = nullptr;   // 通话中的本地小窗
    QWidget* grid_container_ = nullptr;   // 房间模式的多路宫格
    QGridLayout* grid_layout_ = nullptr;
    std::vector<YuvWidget*> grid_widgets_;  // 仅 UI 线程
    QComboBox* device_box_ = nullptr;
    QComboBox* resolution_box_ = nullptr;
    QCheckBox* loopback_box_ = nullptr;
    std::vector<V4l2Capture::DeviceInfo> devices_;
    QTimer title_timer_;
    V4l2Capture capture_;
    uint64_t last_captured_ = 0;
    uint64_t last_rendered_ = 0;

    // 环回链路:采集线程 push → 编解码线程 pop,编码→立即解码→渲染
    BlockingQueue<std::shared_ptr<VideoFrame>> encode_queue_{2};
    std::thread codec_thread_;
    std::unique_ptr<VideoEncoder> encoder_;
    std::unique_ptr<VideoDecoder> decoder_;  // 仅环回模式使用
    std::ofstream dump_file_;
    std::atomic<bool> loopback_on_{false};
    std::atomic<uint64_t> encoded_bytes_{0};
    std::atomic<uint64_t> encoded_frames_{0};
    uint64_t last_encoded_bytes_ = 0;

    // RTP/UDP 传输(mode_ 被采集线程读取,需原子)
    std::atomic<Mode> mode_{Mode::kPreview};
    UdpSocket udp_;
    std::unique_ptr<RtpPacketizer> packetizer_;
    std::thread net_thread_;              // 接收线程
    std::atomic<bool> net_running_{false};
    std::atomic<bool> send_pipeline_on_{false};
    std::atomic<uint64_t> net_bytes_{0};
    std::atomic<uint64_t> net_packets_{0};
    uint64_t last_net_bytes_ = 0;

    // 多路接收:SSRC → 流水线(仅接收线程);UI 线程建好的窗经此发布
    std::map<uint32_t, RemoteStream> remote_streams_;
    std::map<uint32_t, std::unique_ptr<AudioDecoder>> audio_decoders_;
    std::mutex widget_mutex_;
    std::map<uint32_t, YuvWidget*> ready_widgets_;
    // 聚合统计(接收线程写,UI 线程读)
    std::atomic<uint64_t> agg_lost_{0};
    std::atomic<uint64_t> agg_recovered_{0};
    std::atomic<uint64_t> agg_depth_{0};
    std::atomic<uint64_t> agg_skips_{0};

    // 码率自适应(阶段 8):接收端 RR 驱动,发送端调整目标码率
    std::atomic<int> target_bitrate_{0};  // 0 = 未启用/无反馈

    // 弱网对抗(阶段 6)
    rtpctl::RtpHistory send_history_;  // 发送线程写/接收线程读(内部有锁)
    bool nack_enabled_ = true;
    std::atomic<uint64_t> nacks_sent_{0};
    std::atomic<uint64_t> plis_sent_{0};
    std::atomic<uint64_t> retrans_sent_{0};

    // 音频链路(阶段 5):PT=97 复用同一 UDP socket
    AudioCapture audio_capture_;
    AudioPlayer audio_player_;
    std::unique_ptr<AudioEncoder> audio_encoder_;
    uint32_t audio_ssrc_ = 0;
    uint16_t audio_seq_ = 0;  // 仅音频采集线程访问
    std::atomic<uint64_t> audio_tx_packets_{0};
    std::atomic<uint64_t> audio_rx_packets_{0};
    std::atomic<int64_t> last_video_ts_{0};  // 最近解码视频帧的发送端时间戳

    // 信令与呼叫状态
    SignalClient signal_;
    CallState call_state_ = CallState::kIdle;
    QString my_name_;
    QString peer_;         // 当前呼叫/通话对端
    bool auto_answer_ = false;
    QString auto_call_target_;
    bool auto_call_done_ = false;
    QString auto_join_room_;
    std::string room_name_;    // 进房前设置,接收线程 JOIN 保活用
    std::string member_name_;
    int64_t last_join_sent_ = 0;  // 仅接收线程
    QLineEdit* server_edit_ = nullptr;
    QLineEdit* name_edit_ = nullptr;
    QPushButton* login_btn_ = nullptr;
    QComboBox* user_box_ = nullptr;
    QPushButton* call_btn_ = nullptr;
    QPushButton* answer_btn_ = nullptr;
    QPushButton* reject_btn_ = nullptr;
    QPushButton* hangup_btn_ = nullptr;
    QLineEdit* room_edit_ = nullptr;
    QPushButton* join_btn_ = nullptr;
    QLabel* status_label_ = nullptr;
};
