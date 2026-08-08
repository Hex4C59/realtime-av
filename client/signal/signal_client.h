#pragma once

#include <QObject>
#include <QTcpSocket>
#include <cstdint>

#include "common/protocol/signal_message.h"

// 信令客户端:连接信令服务器,收发 [长度][JSON] 帧,转成 Qt 信号。
// 只做传输和编解码,呼叫状态机在 MainWindow 里。
class SignalClient : public QObject {
    Q_OBJECT

public:
    explicit SignalClient(QObject* parent = nullptr);

    void connectToServer(const QString& host, uint16_t port);
    bool isConnected() const;

    void login(const QString& name);
    void call(const QString& to);
    void answer(const QString& to, bool accept);
    void sendMediaInfo(const QString& to, uint16_t udp_port);
    void hangup(const QString& to);
    void joinRoom(const QString& room);

signals:
    void connected();
    void disconnected();
    void loginAck(bool ok, const QString& reason);
    void userList(const QStringList& users);
    void incomingCall(const QString& from);
    void callResult(const QString& peer, bool accept, const QString& reason);
    void peerMedia(const QString& peer, const QString& ip, uint16_t udp_port);
    void peerHangup(const QString& peer);
    void sfuInfo(const QString& ip, uint16_t port, const QString& room);

private:
    void sendMessage(const protocol::json& msg);
    void onReadyRead();

    QTcpSocket socket_;
    protocol::FrameParser parser_;
};
