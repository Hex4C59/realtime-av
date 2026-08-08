#include "client/signal/signal_client.h"

#include <cstdio>

using protocol::json;

SignalClient::SignalClient(QObject* parent) : QObject(parent) {
    connect(&socket_, &QTcpSocket::connected, this, [this] {
        std::fprintf(stderr, "signal: tcp connected\n");
        emit connected();
    });
    connect(&socket_, &QTcpSocket::disconnected, this, &SignalClient::disconnected);
    connect(&socket_, &QTcpSocket::readyRead, this, &SignalClient::onReadyRead);
    connect(&socket_, &QTcpSocket::errorOccurred, this,
            [this](QAbstractSocket::SocketError) {
                std::fprintf(stderr, "signal: socket error: %s\n",
                             qPrintable(socket_.errorString()));
            });
}

void SignalClient::connectToServer(const QString& host, uint16_t port) {
    std::fprintf(stderr, "signal: connecting to %s:%u\n", qPrintable(host), port);
    socket_.abort();
    socket_.connectToHost(host, port);
}

bool SignalClient::isConnected() const {
    return socket_.state() == QAbstractSocket::ConnectedState;
}

void SignalClient::login(const QString& name) {
    sendMessage({{"type", "login"}, {"name", name.toStdString()}});
}

void SignalClient::call(const QString& to) {
    sendMessage({{"type", "call"}, {"to", to.toStdString()}});
}

void SignalClient::answer(const QString& to, bool accept) {
    sendMessage({{"type", "answer"}, {"to", to.toStdString()}, {"accept", accept}});
}

void SignalClient::sendMediaInfo(const QString& to, uint16_t udp_port) {
    sendMessage({{"type", "media_info"}, {"to", to.toStdString()},
                 {"udp_port", udp_port}});
}

void SignalClient::hangup(const QString& to) {
    sendMessage({{"type", "hangup"}, {"to", to.toStdString()}});
}

void SignalClient::joinRoom(const QString& room) {
    sendMessage({{"type", "join_room"}, {"room", room.toStdString()}});
}

void SignalClient::sendMessage(const json& msg) {
    if (!isConnected()) return;
    auto frame = protocol::encodeFrame(msg);
    socket_.write(reinterpret_cast<const char*>(frame.data()),
                  static_cast<qint64>(frame.size()));
}

void SignalClient::onReadyRead() {
    QByteArray data = socket_.readAll();
    bool ok = true;
    auto msgs = parser_.feed(reinterpret_cast<const uint8_t*>(data.constData()),
                             static_cast<size_t>(data.size()), &ok);
    if (!ok) {
        socket_.abort();
        return;
    }
    for (const auto& m : msgs) {
        const std::string type = m.value("type", "");
        if (type == "login_ack") {
            emit loginAck(m.value("ok", false),
                          QString::fromStdString(m.value("reason", "")));
        } else if (type == "user_list") {
            QStringList users;
            for (const auto& u : m.value("users", json::array())) {
                users << QString::fromStdString(u.get<std::string>());
            }
            emit userList(users);
        } else if (type == "incoming_call") {
            emit incomingCall(QString::fromStdString(m.value("from", "")));
        } else if (type == "call_result") {
            emit callResult(QString::fromStdString(m.value("peer", "")),
                            m.value("accept", false),
                            QString::fromStdString(m.value("reason", "")));
        } else if (type == "peer_media") {
            emit peerMedia(QString::fromStdString(m.value("peer", "")),
                           QString::fromStdString(m.value("ip", "")),
                           static_cast<uint16_t>(m.value("udp_port", 0)));
        } else if (type == "peer_hangup") {
            emit peerHangup(QString::fromStdString(m.value("peer", "")));
        } else if (type == "sfu_info") {
            emit sfuInfo(QString::fromStdString(m.value("ip", "")),
                         static_cast<uint16_t>(m.value("port", 0)),
                         QString::fromStdString(m.value("room", "")));
        }
    }
}
