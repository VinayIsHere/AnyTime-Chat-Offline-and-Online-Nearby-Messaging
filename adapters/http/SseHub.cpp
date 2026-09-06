// adapters/http/SseHub.cpp
//
// Implements IEventSink by turning core domain events into SSE frames.

#include "adapters/http/SseHub.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpSocket>
#include <QTimer>

#include <vector>

#include "adapters/http/JsonMapping.h"
#include "core/domain/Types.h"

namespace anytime::http {

struct SseHub::Impl {
    std::vector<QTcpSocket*> clients;
    QTimer                   heartbeat;
    int                      heartbeatMillis = static_cast<int>(kSseHeartbeatMillis);
};

SseHub::SseHub(QObject* parent) : QObject(parent), impl_(std::make_unique<Impl>())
{
    // A comment frame keeps intermediaries from closing an idle stream. It also
    // reveals a socket that died without a FIN, so dead clients get reaped.
    impl_->heartbeat.setInterval(impl_->heartbeatMillis);
    connect(&impl_->heartbeat, &QTimer::timeout, this, [this] {
        broadcastRaw(":keep-alive\n\n");
    });
}

SseHub::~SseHub() = default;

int SseHub::clientCount() const { return static_cast<int>(impl_->clients.size()); }

void SseHub::setHeartbeatInterval(int millis)
{
    impl_->heartbeatMillis = millis;
    impl_->heartbeat.setInterval(millis);
}

void SseHub::attachClient(QTcpSocket* socket)
{
    QByteArray head;
    head += "HTTP/1.1 200 OK\r\n";
    head += "Content-Type: text/event-stream; charset=utf-8\r\n";
    head += "Cache-Control: no-store\r\n";
    // Without this, a proxy may buffer the stream and defeat the whole point.
    head += "X-Accel-Buffering: no\r\n";
    head += "Connection: keep-alive\r\n\r\n";
    socket->write(head);
    socket->flush();

    impl_->clients.push_back(socket);

    connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
        auto& c = impl_->clients;
        c.erase(std::remove(c.begin(), c.end(), socket), c.end());
        socket->deleteLater();
        if (c.empty()) impl_->heartbeat.stop();
    });

    if (!impl_->heartbeat.isActive()) impl_->heartbeat.start();
}

void SseHub::broadcastRaw(const QByteArray& frame)
{
    auto& clients = impl_->clients;

    for (auto it = clients.begin(); it != clients.end();) {
        QTcpSocket* socket = *it;
        if (socket->state() != QAbstractSocket::ConnectedState) {
            it = clients.erase(it);
            continue;
        }
        socket->write(frame);
        socket->flush();
        ++it;
    }
    if (clients.empty()) impl_->heartbeat.stop();
}

void SseHub::broadcast(EventType type, const QJsonObject& payload)
{
    QByteArray frame;
    frame += "event: ";
    frame += toString(type);
    frame += "\ndata: ";
    frame += QJsonDocument(payload).toJson(QJsonDocument::Compact);
    frame += "\n\n";
    broadcastRaw(frame);
}

// ── IEventSink ───────────────────────────────────────────────────────────────

void SseHub::onPeerDiscovered(const Peer& peer)
{
    broadcast(EventType::PeerDiscovered, json::peerToJson(peer));
}

void SseHub::onPeerLost(const PeerId& peerId)
{
    QJsonObject o;
    o.insert(QStringLiteral("peerId"), QString::fromStdString(peerId.toHex()));
    broadcast(EventType::PeerLost, o);
}

void SseHub::onRequestReceived(const anytime::Request& request)
{
    broadcast(EventType::RequestReceived, json::requestToJson(request));
}

void SseHub::onRequestAccepted(const anytime::Request& request, const Chat& chat)
{
    QJsonObject o;
    o.insert(QStringLiteral("request"), json::requestToJson(request));
    o.insert(QStringLiteral("chat"), json::chatToJson(chat, QString()));
    broadcast(EventType::RequestAccepted, o);
}

void SseHub::onRequestDeclined(const RequestId& id)
{
    QJsonObject o;
    o.insert(QStringLiteral("id"), QString::fromStdString(id.value()));
    broadcast(EventType::RequestDeclined, o);
}

void SseHub::onMessageReceived(const Message& message)
{
    broadcast(EventType::MessageReceived, json::messageToJson(message));
}

void SseHub::onMessageStatusChanged(const MessageId& id, DeliveryStatus status)
{
    QJsonObject o;
    o.insert(QStringLiteral("id"), QString::fromStdString(id.value()));
    o.insert(QStringLiteral("status"), QString::fromLatin1(toString(status)));
    broadcast(EventType::MessageStatus, o);
}

void SseHub::onRadioStateChanged(bool available, const std::string& detail)
{
    QJsonObject o;
    o.insert(QStringLiteral("available"), available);
    o.insert(QStringLiteral("detail"), QString::fromStdString(detail));
    broadcast(EventType::RadioState, o);
}

}  // namespace anytime::http
