// adapters/transport/loopback/LoopbackTransport.cpp
//
// Read the header for why this is a deliverable rather than a test double (AD-07).
//
// Wire framing on a loopback link is a 4-byte big-endian length prefix followed
// by the frame. TCP is a stream, so without a prefix two announces written back
// to back would arrive as one read and be parsed as garbage.

#include "adapters/transport/loopback/LoopbackTransport.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <cstring>
#include <unordered_map>
#include <vector>

#include "core/log/Log.h"

namespace anytime::transport {

namespace {

constexpr int kLengthPrefixBytes = 4;

void appendLengthPrefixed(QByteArray& out, ByteSpan frame)
{
    const quint32 size = static_cast<quint32>(frame.size());
    out.append(static_cast<char>((size >> 24) & 0xFF));
    out.append(static_cast<char>((size >> 16) & 0xFF));
    out.append(static_cast<char>((size >> 8) & 0xFF));
    out.append(static_cast<char>(size & 0xFF));
    out.append(reinterpret_cast<const char*>(frame.data()), static_cast<int>(frame.size()));
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────

struct LoopbackTransport::Impl {
    LoopbackConfig      config;
    ITransportListener* listener = nullptr;
    PeerId              localPeerId;

    QTcpServer server;
    QTimer     scanTimer;

    bool started      = false;
    bool advertising  = false;
    bool discovering  = false;

    LinkId nextLinkId = 1;

    struct Link {
        QTcpSocket* socket = nullptr;
        PeerId      peerId;
        QByteArray  buffer;
    };
    std::unordered_map<LinkId, Link> links;
    std::unordered_map<QTcpSocket*, LinkId> bySocket;

    /// peerId hex -> port, learned by scanning the rendezvous directory.
    std::unordered_map<std::string, quint16> seenPeers;

    QString registrationFile() const
    {
        return config.rendezvousDir + QLatin1Char('/')
               + QString::fromStdString(localPeerId.toHex()) + QStringLiteral(".peer");
    }

    void publishRegistration();
    void removeRegistration();
    void scanRendezvous();
    void acceptConnections();
    void readFrom(QTcpSocket*);
    LinkId adopt(QTcpSocket*, const PeerId&);
    void   dropLink(LinkId, LinkCloseReason);

    bool shouldDropFrame() const;
    int  deliveryDelay() const;
};

// ─────────────────────────────────────────────────────────────────────────────

LoopbackTransport::LoopbackTransport(LoopbackConfig config, QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>())
{
    impl_->config = std::move(config);
    if (impl_->config.rendezvousDir.isEmpty()) {
        impl_->config.rendezvousDir = QDir::tempPath() + QStringLiteral("/anytime-loopback");
    }
}

LoopbackTransport::~LoopbackTransport()
{
    stop();
}

TransportCapabilities LoopbackTransport::caps() const
{
    TransportCapabilities caps;
    // Deliberately mirrors BLE's frame budget. If loopback allowed bigger frames
    // the fragmentation path would never be exercised in development, and the
    // first time it ran would be on a handset.
    caps.maxFramePayload = protocol::kFragmentPayloadBytes;
    caps.throughputClass = 0;
    caps.supportsBulk = false;
    caps.supportsMesh = true;
    caps.requiresUserConsent = false;
    caps.energyCost = EnergyCost::Low;
    return caps;
}

bool LoopbackTransport::isAvailable() const { return impl_->started; }

void LoopbackTransport::setListener(ITransportListener* listener) { impl_->listener = listener; }

void LoopbackTransport::setImpairment(const Impairment& impairment)
{
    impl_->config.impairment = impairment;
}

Result LoopbackTransport::start(const PeerId& localPeerId)
{
    if (impl_->started) return Result::ok();

    impl_->localPeerId = localPeerId;

    QDir().mkpath(impl_->config.rendezvousDir);

    if (!impl_->server.listen(QHostAddress::LocalHost, 0)) {
        AT_LOG_ERROR("loopback", "could not listen: {}",
                     impl_->server.errorString().toStdString());
        return Result::error(ErrorCode::TransportUnavailable,
                             impl_->server.errorString().toStdString());
    }

    connect(&impl_->server, &QTcpServer::newConnection, this,
            [this] { impl_->acceptConnections(); });

    connect(&impl_->scanTimer, &QTimer::timeout, this, [this] { impl_->scanRendezvous(); });
    impl_->scanTimer.setInterval(impl_->config.scanIntervalMillis);

    impl_->started = true;
    AT_LOG_INFO("loopback", "listening on 127.0.0.1:{}, rendezvous {}",
                impl_->server.serverPort(), impl_->config.rendezvousDir.toStdString());
    if (impl_->listener) impl_->listener->onTransportStateChanged(id(), true);
    return Result::ok();
}

void LoopbackTransport::stop()
{
    if (!impl_->started) return;

    impl_->scanTimer.stop();
    impl_->removeRegistration();
    impl_->server.close();

    for (auto& [linkId, link] : impl_->links) {
        if (link.socket) link.socket->abort();
    }
    impl_->links.clear();
    impl_->bySocket.clear();

    impl_->started = false;
    impl_->advertising = false;
    impl_->discovering = false;
}

Result LoopbackTransport::startDiscovery(DiscoveryMode mode)
{
    if (!impl_->started) {
        return Result::error(ErrorCode::TransportUnavailable, "Transport not started.");
    }
    if (mode == DiscoveryMode::Off) {
        stopDiscovery();
        return Result::ok();
    }
    impl_->discovering = true;
    impl_->scanRendezvous();
    impl_->scanTimer.start();
    return Result::ok();
}

void LoopbackTransport::stopDiscovery()
{
    impl_->discovering = false;
    impl_->scanTimer.stop();
}

Result LoopbackTransport::startAdvertising()
{
    if (!impl_->started) {
        return Result::error(ErrorCode::TransportUnavailable, "Transport not started.");
    }
    impl_->advertising = true;
    impl_->publishRegistration();
    return Result::ok();
}

void LoopbackTransport::stopAdvertising()
{
    impl_->advertising = false;
    impl_->removeRegistration();
}

Outcome<LinkId> LoopbackTransport::openLink(const PeerHandle& handle)
{
    bool ok = false;
    const quint16 port =
        static_cast<quint16>(QString::fromStdString(handle.address).toUShort(&ok));
    if (!ok || port == 0) {
        return Outcome<LinkId>::error(ErrorCode::InvalidArgument, "Bad loopback address.");
    }

    auto* socket = new QTcpSocket(this);
    socket->connectToHost(QHostAddress::LocalHost, port);
    if (!socket->waitForConnected(500)) {
        AT_LOG_DEBUG("loopback", "{} on port {} is not accepting links", handle.peerId.toHex(),
                     port);
        socket->deleteLater();
        return Outcome<LinkId>::error(ErrorCode::LinkClosed, "Peer is not accepting links.");
    }

    // Announce who we are, so the far side can label the link without guessing.
    QByteArray hello(reinterpret_cast<const char*>(impl_->localPeerId.bytes().data()),
                     static_cast<int>(kPeerIdBytes));
    socket->write(hello);
    socket->flush();

    return impl_->adopt(socket, handle.peerId);
}

void LoopbackTransport::closeLink(LinkId link) { impl_->dropLink(link, LinkCloseReason::Local); }

Result LoopbackTransport::send(LinkId link, ByteSpan frame)
{
    if (frame.size() > caps().maxFramePayload) {
        return Result::error(ErrorCode::InvalidArgument, "Frame exceeds the transport budget.");
    }
    if (impl_->shouldDropFrame()) return Result::ok();  // simulated loss: silently gone

    QByteArray wire;
    appendLengthPrefixed(wire, frame);

    // kInvalidLink means broadcast — how announces reach every open link.
    if (link == kInvalidLink) {
        for (auto& [id, entry] : impl_->links) {
            if (entry.socket) entry.socket->write(wire);
        }
        return Result::ok();
    }

    const auto it = impl_->links.find(link);
    if (it == impl_->links.end() || !it->second.socket) {
        return Result::error(ErrorCode::LinkClosed, "No such link.");
    }
    it->second.socket->write(wire);
    return Result::ok();
}

// ─────────────────────────────────────────────────────────────────────────────

bool LoopbackTransport::Impl::shouldDropFrame() const
{
    if (config.impairment.lossRate <= 0.0) return false;
    return QRandomGenerator::global()->generateDouble() < config.impairment.lossRate;
}

int LoopbackTransport::Impl::deliveryDelay() const
{
    const Impairment& i = config.impairment;
    if (i.latencyMillis <= 0 && i.jitterMillis <= 0) return 0;
    const int jitter =
        i.jitterMillis > 0 ? QRandomGenerator::global()->bounded(i.jitterMillis * 2) - i.jitterMillis
                           : 0;
    return std::max(0, i.latencyMillis + jitter);
}

void LoopbackTransport::Impl::publishRegistration()
{
    QJsonObject o;
    o.insert(QStringLiteral("peerId"), QString::fromStdString(localPeerId.toHex()));
    o.insert(QStringLiteral("port"), server.serverPort());

    QFile file(registrationFile());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    file.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

void LoopbackTransport::Impl::removeRegistration()
{
    QFile::remove(registrationFile());
}

void LoopbackTransport::Impl::scanRendezvous()
{
    if (!discovering) return;

    // Republish on every scan: a crashed instance leaves a stale file behind,
    // and this keeps a live one from being mistaken for it.
    if (advertising) publishRegistration();

    QDir dir(config.rendezvousDir);
    const QStringList entries = dir.entryList({QStringLiteral("*.peer")}, QDir::Files);

    for (const QString& entry : entries) {
        QFile file(dir.filePath(entry));
        if (!file.open(QIODevice::ReadOnly)) continue;

        const QJsonObject o = QJsonDocument::fromJson(file.readAll()).object();
        const QString hex = o.value(QStringLiteral("peerId")).toString();
        const quint16 port = static_cast<quint16>(o.value(QStringLiteral("port")).toInt());
        if (hex.isEmpty() || port == 0) continue;

        const auto peerId = PeerId::fromHex(hex.toStdString());
        if (!peerId || *peerId == localPeerId) continue;

        seenPeers[hex.toStdString()] = port;

        PeerHandle handle;
        handle.peerId = *peerId;
        handle.address = QString::number(port).toStdString();
        handle.rssi = config.simulatedRssi;

        // Report the sighting and nothing more. Whether to connect is decided in
        // the core (DiscoveryService), exactly as it is for BLE. This transport
        // used to connect on sight, which made desktop testing pass while the
        // BLE path — which has no such shortcut — never opened a link at all.
        if (listener) listener->onPeerSeen(TransportId::Loopback, handle);
    }
}

void LoopbackTransport::Impl::acceptConnections()
{
    while (QTcpSocket* socket = server.nextPendingConnection()) {
        // The peer id arrives as the first 8 bytes; adopt once it is here.
        QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
            if (bySocket.count(socket)) { readFrom(socket); return; }

            if (socket->bytesAvailable() < static_cast<qint64>(kPeerIdBytes)) return;

            const QByteArray hello = socket->read(static_cast<qint64>(kPeerIdBytes));
            PeerIdBytes bytes{};
            std::memcpy(bytes.data(), hello.constData(), kPeerIdBytes);
            adopt(socket, PeerId{bytes});
            readFrom(socket);
        });
    }
}

LinkId LoopbackTransport::Impl::adopt(QTcpSocket* socket, const PeerId& peerId)
{
    const LinkId id = nextLinkId++;

    Link link;
    link.socket = socket;
    link.peerId = peerId;
    links[id] = link;
    bySocket[socket] = id;

    QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket] { readFrom(socket); });
    QObject::connect(socket, &QTcpSocket::disconnected, socket, [this, socket] {
        const auto it = bySocket.find(socket);
        if (it != bySocket.end()) dropLink(it->second, LinkCloseReason::Remote);
    });

    if (listener) listener->onLinkUp(TransportId::Loopback, id, peerId);
    return id;
}

void LoopbackTransport::Impl::readFrom(QTcpSocket* socket)
{
    const auto found = bySocket.find(socket);
    if (found == bySocket.end()) return;

    Link& link = links[found->second];
    link.buffer.append(socket->readAll());

    // Drain every complete frame the buffer holds — a single read can carry
    // several, and leaving one behind would stall until the next arrival.
    for (;;) {
        if (link.buffer.size() < kLengthPrefixBytes) return;

        const auto* p = reinterpret_cast<const quint8*>(link.buffer.constData());
        const quint32 size = (quint32(p[0]) << 24) | (quint32(p[1]) << 16) | (quint32(p[2]) << 8)
                             | quint32(p[3]);

        if (size > protocol::kMaxReassembledBytes) {  // nonsense length: resync is impossible
            AT_LOG_WARN("loopback", "link {} sent a {}-byte frame length; closing it",
                        found->second, size);
            dropLink(found->second, LinkCloseReason::Error);
            return;
        }
        if (link.buffer.size() < static_cast<int>(kLengthPrefixBytes + size)) return;

        const QByteArray frame = link.buffer.mid(kLengthPrefixBytes, static_cast<int>(size));
        link.buffer.remove(0, kLengthPrefixBytes + static_cast<int>(size));

        if (shouldDropFrame()) continue;

        const int delay = deliveryDelay();
        const LinkId linkId = found->second;

        if (delay == 0) {
            if (listener) {
                listener->onFrame(TransportId::Loopback, linkId,
                                  ByteSpan{reinterpret_cast<const Byte*>(frame.constData()),
                                           static_cast<std::size_t>(frame.size())});
            }
        } else {
            // Copy: the caller's buffer is gone by the time the timer fires.
            QTimer::singleShot(delay, socket, [this, linkId, frame] {
                if (listener) {
                    listener->onFrame(TransportId::Loopback, linkId,
                                      ByteSpan{reinterpret_cast<const Byte*>(frame.constData()),
                                               static_cast<std::size_t>(frame.size())});
                }
            });
        }
    }
}

void LoopbackTransport::Impl::dropLink(LinkId id, LinkCloseReason reason)
{
    const auto it = links.find(id);
    if (it == links.end()) return;

    QTcpSocket* socket = it->second.socket;
    if (socket) {
        bySocket.erase(socket);
        socket->deleteLater();
    }
    links.erase(it);

    if (listener) listener->onLinkDown(TransportId::Loopback, id, reason);
}

}  // namespace anytime::transport
