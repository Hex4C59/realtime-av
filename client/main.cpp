#include <QApplication>
#include <QSurfaceFormat>
#include <QElapsedTimer>
#include <QTimer>
#include <cstdio>

#include "client/ui/main_window.h"

// 用法: avclient [/dev/videoN] [--loopback] [--send ip:port] [--recv port]
//                [--server ip --user 用户名] [--call 对端名] [--auto-answer]
//                (注意:不能叫 --name,那是 Qt/X11 内置参数,会被 QApplication 吃掉)
//                [--no-camera] [--self-test out.png] [--self-test-delay 毫秒]
// --loopback:    启动即开启编码环回(采集→H.264→解码→渲染,录制 loopback.h264)
// --send/--recv: 阶段 3 无信令单向传输(--recv 不占摄像头)
// --server:      连接信令服务器并用 --user 登录
// --join ROOM:   登录后自动加入 SFU 房间(多人会议)
// --call:        目标用户上线后自动呼叫(验收脚本用)
// --auto-answer: 来电自动接听(验收脚本用)
// --self-test:   延时后(默认 3 秒)把渲染画面存为 PNG 并退出,
//                有帧退 0,无帧退 1,用于无人值守的链路验收
int main(int argc, char* argv[]) {
    // 必须在创建 QApplication 前设置,shader 用的是 GLSL 330 core
    QSurfaceFormat fmt;
    fmt.setVersion(3, 3);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    QSurfaceFormat::setDefaultFormat(fmt);

    QApplication app(argc, argv);

    QString device;
    QString self_test_path;
    QString send_target;  // ip:port
    QString server, name, call_target, join_room;
    int recv_port = -1;
    int self_test_delay = 3000;
    bool loopback = false;
    bool auto_answer = false;
    bool no_camera = false;
    bool no_nack = false;
    for (int i = 1; i < argc; ++i) {
        QString arg = argv[i];
        if (arg == "--self-test" && i + 1 < argc) {
            self_test_path = argv[++i];
        } else if (arg == "--self-test-delay" && i + 1 < argc) {
            self_test_delay = QString(argv[++i]).toInt();
        } else if (arg == "--send" && i + 1 < argc) {
            send_target = argv[++i];
        } else if (arg == "--recv" && i + 1 < argc) {
            recv_port = QString(argv[++i]).toInt();
        } else if (arg == "--server" && i + 1 < argc) {
            server = argv[++i];
        } else if (arg == "--user" && i + 1 < argc) {
            name = argv[++i];
        } else if (arg == "--call" && i + 1 < argc) {
            call_target = argv[++i];
        } else if (arg == "--join" && i + 1 < argc) {
            join_room = argv[++i];
        } else if (arg == "--auto-answer") {
            auto_answer = true;
        } else if (arg == "--no-camera") {
            no_camera = true;
        } else if (arg == "--loopback") {
            loopback = true;
        } else if (arg == "--no-nack") {
            no_nack = true;
        } else if (arg.startsWith("/dev/")) {
            device = arg;
        }
    }

    MainWindow window(/*open_camera=*/recv_port < 0 && !no_camera);
    if (recv_port < 0 && !no_camera && !device.isEmpty() &&
        !window.selectDevice(device)) {
        std::fprintf(stderr, "device %s not found\n", qPrintable(device));
        return 1;
    }
    if (loopback) window.setLoopback(true);
    if (no_nack) window.setNackEnabled(false);
    if (!server.isEmpty() && !name.isEmpty()) {
        window.autoConnect(server, name, call_target, auto_answer, join_room);
    }
    if (!send_target.isEmpty()) {
        QStringList parts = send_target.split(':');
        if (parts.size() != 2 ||
            !window.startSend(parts[0], static_cast<uint16_t>(parts[1].toUInt()))) {
            std::fprintf(stderr, "bad --send target or start failed: %s\n",
                         qPrintable(send_target));
            return 1;
        }
    }
    if (recv_port >= 0 && !window.startRecv(static_cast<uint16_t>(recv_port))) {
        std::fprintf(stderr, "cannot listen on port %d\n", recv_port);
        return 1;
    }
    window.show();

    QElapsedTimer elapsed;
    elapsed.start();
    if (!self_test_path.isEmpty()) {
        QTimer::singleShot(self_test_delay, [&, elapsed] {
            std::fprintf(stderr, "self-test: timer fired at %lld ms\n",
                         static_cast<long long>(elapsed.elapsed()));
            // 抓整窗(含宫格多路画面),QWidget::grab 会合成 GL 子窗内容
            QImage img = window.grab().toImage();
            bool ok = window.totalRenderedFrames() > 0 && img.save(self_test_path);
            std::fprintf(stderr, "self-test: rendered=%llu saved=%d at %lld ms\n",
                         static_cast<unsigned long long>(
                             window.totalRenderedFrames()),
                         ok, static_cast<long long>(elapsed.elapsed()));
            // 抓图后延迟退出:双实例互测时,先退出的一方会触发对端挂断、
            // 对端画面切回本地预览,若对端还没抓图就会抓错内容
            QTimer::singleShot(2000, [&app, ok] { app.exit(ok ? 0 : 1); });
        });
    }
    int rc = app.exec();
    std::fprintf(stderr, "event loop exited rc=%d at %lld ms\n", rc,
                 static_cast<long long>(elapsed.elapsed()));
    return rc;
}
