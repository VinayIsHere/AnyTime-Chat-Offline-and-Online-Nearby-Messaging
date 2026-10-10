// adapters/http/JsonMapping.cpp

#include "adapters/http/JsonMapping.h"

#include <QDateTime>

namespace anytime::http::json {

QString handleFor(const std::string& nickname, const PeerId& peerId)
{
    const QString suffix = QString::fromStdString(peerId.shortLabel());
    if (nickname.empty()) return QLatin1Char('@') + suffix;
    // U+00B7 MIDDLE DOT — quieter than '#', and not a character usernames allow.
    return QLatin1Char('@') + QString::fromStdString(nickname) + QChar(0x00B7) + suffix;
}

QString relativeTime(Timestamp then, Timestamp now)
{
    if (then == kNoTimestamp) return QString();

    const qint64 seconds = (now - then) / 1000;
    if (seconds < 45)          return QStringLiteral("now");
    if (seconds < 3600)        return QStringLiteral("%1m").arg(seconds / 60);
    if (seconds < 86400)       return QStringLiteral("%1h").arg(seconds / 3600);
    if (seconds < 7 * 86400)   return QStringLiteral("%1d").arg(seconds / 86400);

    return QDateTime::fromMSecsSinceEpoch(then).toString(QStringLiteral("d MMM"));
}

QJsonObject accountToJson(const Account& a)
{
    QJsonObject o;
    o.insert(QStringLiteral("peerId"), QString::fromStdString(a.peerId.toHex()));
    o.insert(QStringLiteral("username"), QString::fromStdString(a.username));
    o.insert(QStringLiteral("handle"), handleFor(a.username, a.peerId));
    o.insert(QStringLiteral("dob"), QString::fromStdString(a.dateOfBirth));
    o.insert(QStringLiteral("gender"), QString::fromLatin1(toString(a.gender)));
    o.insert(QStringLiteral("discoverable"), a.discoverable);
    o.insert(QStringLiteral("acceptFrom"),
             a.acceptFrom == AcceptFrom::Everyone ? QStringLiteral("everyone")
                                                  : QStringLiteral("nobody"));
    return o;
}

QJsonObject peerToJson(const Peer& p, PeerStatus status, bool canResend, const QString& chatId)
{
    QJsonObject o;
    o.insert(QStringLiteral("peerId"), QString::fromStdString(p.peerId.toHex()));
    o.insert(QStringLiteral("nickname"), QString::fromStdString(p.nickname));
    o.insert(QStringLiteral("handle"), handleFor(p.nickname, p.peerId));
    o.insert(QStringLiteral("signal"), QString::fromLatin1(toString(p.signal)));
    o.insert(QStringLiteral("status"), QString::fromLatin1(toString(status)));
    o.insert(QStringLiteral("canResend"), canResend);
    if (!chatId.isEmpty()) o.insert(QStringLiteral("chatId"), chatId);
    return o;
}

QJsonObject requestToJson(const Request& r)
{
    QJsonObject o;
    o.insert(QStringLiteral("id"), QString::fromStdString(r.id.value()));
    o.insert(QStringLiteral("peerId"), QString::fromStdString(r.peerId.toHex()));
    o.insert(QStringLiteral("nickname"), QString::fromStdString(r.nickname));
    o.insert(QStringLiteral("handle"), handleFor(r.nickname, r.peerId));
    o.insert(QStringLiteral("greeting"), QString::fromStdString(r.greeting));
    o.insert(QStringLiteral("at"),
             relativeTime(r.createdAt, QDateTime::currentMSecsSinceEpoch()));
    return o;
}

QJsonObject chatToJson(const Chat& c, const QString& handle)
{
    QJsonObject o;
    o.insert(QStringLiteral("id"), QString::fromStdString(c.id.value()));
    o.insert(QStringLiteral("peerId"), QString::fromStdString(c.peerId.toHex()));
    o.insert(QStringLiteral("handle"), handle);
    o.insert(QStringLiteral("lastMessage"), QString());
    o.insert(QStringLiteral("at"),
             relativeTime(c.lastMessageAt, QDateTime::currentMSecsSinceEpoch()));
    o.insert(QStringLiteral("unread"), c.unreadCount);
    o.insert(QStringLiteral("reachable"), false);
    return o;
}

QJsonObject chatSummaryToJson(const ChatSummary& c)
{
    QJsonObject o;
    o.insert(QStringLiteral("id"), QString::fromStdString(c.id.value()));
    o.insert(QStringLiteral("peerId"), QString::fromStdString(c.peerId.toHex()));
    o.insert(QStringLiteral("handle"), handleFor(c.displayName, c.peerId));
    o.insert(QStringLiteral("lastMessage"), QString::fromStdString(c.lastMessage));
    o.insert(QStringLiteral("at"),
             relativeTime(c.lastMessageAt, QDateTime::currentMSecsSinceEpoch()));
    o.insert(QStringLiteral("unread"), c.unreadCount);
    o.insert(QStringLiteral("reachable"), c.reachable);
    o.insert(QStringLiteral("blocked"), c.blocked);
    return o;
}

QJsonObject blockedPeerToJson(const BlockedPeer& b)
{
    QJsonObject o;
    o.insert(QStringLiteral("peerId"), QString::fromStdString(b.peerId.toHex()));
    o.insert(QStringLiteral("nickname"), QString::fromStdString(b.displayName));
    o.insert(QStringLiteral("handle"), handleFor(b.displayName, b.peerId));
    o.insert(QStringLiteral("at"), relativeTime(b.blockedAt, QDateTime::currentMSecsSinceEpoch()));
    return o;
}

QJsonObject messageToJson(const Message& m)
{
    QJsonObject o;
    o.insert(QStringLiteral("id"), QString::fromStdString(m.id.value()));
    o.insert(QStringLiteral("chatId"), QString::fromStdString(m.chatId.value()));
    o.insert(QStringLiteral("clientMessageId"),
             QString::fromStdString(m.clientMessageId.value()));
    o.insert(QStringLiteral("body"), QString::fromStdString(m.body));
    o.insert(QStringLiteral("mine"), m.isMine());
    o.insert(QStringLiteral("at"),
             relativeTime(m.createdAt, QDateTime::currentMSecsSinceEpoch()));
    o.insert(QStringLiteral("status"), QString::fromLatin1(toString(m.status)));

    // Media is a reference, never the bytes: the UI fetches the image from
    // GET /media/:id, so a chat list of fifty photos is fifty small rows rather
    // than a multi-megabyte JSON document.
    if (m.isMedia()) {
        QJsonObject media;
        media.insert(QStringLiteral("id"), QString::fromStdString(m.mediaId));
        media.insert(QStringLiteral("mimeType"), QString::fromStdString(m.mediaMimeType));
        media.insert(QStringLiteral("bytes"), static_cast<qint64>(m.mediaBytes));
        o.insert(QStringLiteral("media"), media);
    }

    return o;
}

}  // namespace anytime::http::json
