// adapters/http/SseHub.h
//
// Server-Sent Events on GET /{token}/api/events. Implements IEventSink, so the
// core emits domain events and they arrive in the browser as SSE frames.
//
// This is what replaces the 2.5 s polling loop in the NearbyScreen draft. One
// long-lived response per client; the core pushes.
//
// SSE rather than WebSocket because the channel is strictly one-way — the UI
// already has HTTP for everything it sends. No upgrade handshake, no framing,
// no ping/pong, and it reconnects on its own.

#pragma once

#include <QJsonObject>
#include <QObject>

#include <memory>
#include <string>

#include "core/ports/IEventSink.h"

class QTcpSocket;

namespace anytime::http {

class SseHub : public QObject, public IEventSink {
    Q_OBJECT

public:
    explicit SseHub(QObject* parent = nullptr);
    ~SseHub() override;

    /// Takes over the socket, writes the SSE response headers and keeps it open.
    /// The hub owns the socket from this point.
    void attachClient(QTcpSocket*);

    int clientCount() const;

    /// Keeps intermediaries from closing an idle stream. Started with the first
    /// client, stopped with the last.
    void setHeartbeatInterval(int millis);

    // ── IEventSink ───────────────────────────────────────────────────────────
    void onPeerDiscovered(const Peer&) override;
    void onPeerLost(const PeerId&) override;
    // NOTE the qualification: inside namespace anytime::http, a bare `Request`
    // resolves to http::Request (the parsed HTTP request), not the domain entity.
    // Without anytime:: these silently fail to override IEventSink.
    void onRequestReceived(const anytime::Request&) override;
    void onRequestAccepted(const anytime::Request&, const Chat&) override;
    void onRequestDeclined(const RequestId&, const PeerId&) override;
    void onMessageReceived(const Message&) override;
    void onMessageStatusChanged(const MessageId&, DeliveryStatus) override;
    void onRadioStateChanged(bool available, const std::string& detail) override;

private:
    /// Writes one `event:` / `data:` frame to every attached client.
    void broadcast(EventType, const QJsonObject& payload);

    /// Writes a pre-formed frame and reaps sockets that have gone away.
    void broadcastRaw(const QByteArray& frame);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace anytime::http
