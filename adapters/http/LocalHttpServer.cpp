// adapters/http/LocalHttpServer.cpp
//
// See the header for the licence reasoning (AD-03) and the four-part security
// model (AD-08). This file implements that model; read it alongside
// docs/ARCHITECTURE.md section 10.

#include "adapters/http/LocalHttpServer.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>

#include "adapters/http/ApiRoutes.h"
#include "adapters/http/Router.h"
#include "adapters/http/SseHub.h"
#include "core/AppCore.h"
#include "core/domain/Types.h"
#include "core/log/Log.h"

// Nothing in this file logs a request path before the prefix is stripped, or the
// token itself: the token IS the credential, and a log file is easy to share.

namespace anytime::http {

namespace {

/// Generates the secret path prefix. QRandomGenerator::system() is the OS CSPRNG
/// — never the default generator, which is seeded predictably.
QString generateToken()
{
    QByteArray raw(static_cast<int>(kApiTokenBytes), Qt::Uninitialized);
    QRandomGenerator::system()->generate(reinterpret_cast<quint32*>(raw.data()),
                                         reinterpret_cast<quint32*>(raw.data() + raw.size()));
    return QString::fromLatin1(raw.toHex());
}

QByteArray mimeForPath(const QString& path)
{
    if (path.endsWith(QLatin1String(".html"))) return "text/html; charset=utf-8";
    if (path.endsWith(QLatin1String(".js")))   return "application/javascript; charset=utf-8";
    if (path.endsWith(QLatin1String(".css")))  return "text/css; charset=utf-8";
    if (path.endsWith(QLatin1String(".json"))) return "application/json; charset=utf-8";
    if (path.endsWith(QLatin1String(".svg")))  return "image/svg+xml";
    if (path.endsWith(QLatin1String(".png")))  return "image/png";
    if (path.endsWith(QLatin1String(".jpg")) || path.endsWith(QLatin1String(".jpeg")))
        return "image/jpeg";
    if (path.endsWith(QLatin1String(".woff2"))) return "font/woff2";
    if (path.endsWith(QLatin1String(".ico")))   return "image/x-icon";
    return "application/octet-stream";
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────

struct LocalHttpServer::Impl {
    AppCore&     core;
    SseHub&      sse;      // borrowed: the core's IEventSink, not a second hub
    ServerConfig config;
    QTcpServer   server;
    Router       router;
    QString      token;

    Impl(AppCore& c, SseHub& hub, ServerConfig cfg)
        : core(c), sse(hub), config(std::move(cfg)) {}

    QString basePath() const { return QLatin1Char('/') + token + QLatin1Char('/'); }

    void   onNewConnection();
    void   handleBuffer(QTcpSocket*);
    void   respond(QTcpSocket*, const Response&);

    /// Splits the secret prefix off the request path, or rejects.
    bool   stripPrefix(QString& path) const;
    bool   originAllowed(const Request&) const;

    Response serveAsset(const QString& relativePath) const;
};

// ─────────────────────────────────────────────────────────────────────────────

LocalHttpServer::LocalHttpServer(AppCore& core, SseHub& sse, ServerConfig config,
                                 QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>(core, sse, std::move(config)))
{
}

LocalHttpServer::~LocalHttpServer() = default;

quint16 LocalHttpServer::port() const { return impl_->server.serverPort(); }
QString LocalHttpServer::token() const { return impl_->token; }

QString LocalHttpServer::rootUrl() const
{
    return QStringLiteral("http://127.0.0.1:%1%2").arg(port()).arg(impl_->basePath());
}

bool LocalHttpServer::start()
{
    impl_->token = generateToken();

    registerApiRoutes(impl_->router, impl_->core, impl_->sse);

    QObject::connect(&impl_->server, &QTcpServer::newConnection, this,
                     [this] { impl_->onNewConnection(); });

    // 127.0.0.1 only. Never QHostAddress::Any — that would put the whole chat
    // history on the local network.
    if (!impl_->server.listen(QHostAddress::LocalHost, impl_->config.port)) {
        AT_LOG_ERROR("http", "local API failed to bind: {}",
                     impl_->server.errorString().toStdString());
        return false;
    }
    AT_LOG_INFO("http", "local API listening on 127.0.0.1:{}", impl_->server.serverPort());
    return true;
}

void LocalHttpServer::stop()
{
    impl_->server.close();
}

// ─────────────────────────────────────────────────────────────────────────────

void LocalHttpServer::Impl::onNewConnection()
{
    while (QTcpSocket* socket = server.nextPendingConnection()) {
        // Defence in depth: refuse anything that is not loopback, in case a
        // future change ever widens the bind address.
        if (config.requireLoopbackPeer && !socket->peerAddress().isLoopback()) {
            AT_LOG_WARN("http", "refused a connection from non-loopback address {}",
                        socket->peerAddress().toString().toStdString());
            socket->abort();
            socket->deleteLater();
            continue;
        }

        socket->setProperty("buffer", QByteArray());

        QObject::connect(socket, &QTcpSocket::readyRead, socket,
                         [this, socket] { handleBuffer(socket); });
        QObject::connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
    }
}

void LocalHttpServer::Impl::handleBuffer(QTcpSocket* socket)
{
    QByteArray buffer = socket->property("buffer").toByteArray();
    buffer.append(socket->readAll());
    socket->setProperty("buffer", buffer);

    Request request;
    int consumed = 0;
    const ParseState state = parseRequest(buffer, request, consumed);

    if (state == ParseState::NeedMoreData) return;
    if (state == ParseState::Malformed) {
        AT_LOG_WARN("http", "malformed request ({} bytes)", buffer.size());
        respond(socket, Response::error(400, "invalid_argument", QStringLiteral("Malformed request.")));
        return;
    }

    // ── Gate 1: the secret path prefix (AD-08) ──────────────────────────────
    // Anything without it gets a bare 404 — no hint that a server is here at all.
    QString path = request.path;
    if (!stripPrefix(path)) {
        // Path deliberately not logged — see the note at the top of the file.
        AT_LOG_DEBUG("http", "request without the API prefix refused");
        respond(socket, Response{404, "text/plain", "Not Found", false});
        return;
    }
    request.path = path;

    // ── Gate 2: Host / Origin, against DNS rebinding ────────────────────────
    if (!originAllowed(request)) {
        AT_LOG_WARN("http", "rejected foreign Host/Origin (host '{}', origin '{}')",
                    request.header("host").toStdString(), request.header("origin").toStdString());
        respond(socket, Response::error(403, "permission_denied", QStringLiteral("Rejected origin.")));
        return;
    }

    // ── SSE stream: hand the socket to the hub and stop managing it here ────
    if (request.method == Method::Get && path == QLatin1String("api/events")) {
        QObject::disconnect(socket, &QTcpSocket::readyRead, nullptr, nullptr);
        sse.attachClient(socket);
        return;
    }

    // ── API ─────────────────────────────────────────────────────────────────
    if (path.startsWith(QLatin1String("api/"))) {
        Request apiRequest = request;
        apiRequest.path = path.mid(4);  // strip "api/"

        if (auto response = router.dispatch(apiRequest)) {
            // Path is safe to log here: the secret prefix is already stripped.
            if (response->status >= 500) {
                AT_LOG_ERROR("http", "{} api/{} -> {}",
                             methodName(apiRequest.method).toStdString(),
                             apiRequest.path.toStdString(), response->status);
            } else if (response->status >= 400) {
                AT_LOG_DEBUG("http", "{} api/{} -> {}",
                             methodName(apiRequest.method).toStdString(),
                             apiRequest.path.toStdString(), response->status);
            }
            respond(socket, *response);
            return;
        }
        respond(socket,
                router.pathExistsForOtherMethod(apiRequest)
                    ? Response::error(405, "invalid_argument", QStringLiteral("Method not allowed."))
                    : Response::error(404, "not_found", QStringLiteral("No such endpoint.")));
        return;
    }

    // ── Static assets ───────────────────────────────────────────────────────
    respond(socket, serveAsset(path));
}

bool LocalHttpServer::Impl::stripPrefix(QString& path) const
{
    const QString prefix = basePath();
    if (!path.startsWith(prefix)) return false;
    path = path.mid(prefix.size());
    return true;
}

bool LocalHttpServer::Impl::originAllowed(const Request& request) const
{
    // The WebView sends Host: 127.0.0.1:<port>. A rebinding attack arrives with
    // an attacker-controlled hostname, so anything not loopback is refused.
    const QByteArray host = request.header("host");
    if (!host.isEmpty()) {
        const QByteArray name = host.left(host.indexOf(':'));
        if (name != "127.0.0.1" && name != "localhost" && name != "[::1]") return false;
    }

    // Same-origin fetches from our own page send no Origin header at all; an
    // Origin from anywhere else is not ours.
    const QByteArray origin = request.header("origin");
    if (!origin.isEmpty() && !origin.startsWith("http://127.0.0.1")
        && !origin.startsWith("http://localhost")) {
        return false;
    }
    return true;
}

Response LocalHttpServer::Impl::serveAsset(const QString& relativePath) const
{
    QString name = relativePath;
    if (name.isEmpty()) name = QStringLiteral("index.html");

    // Reject traversal before touching the filesystem.
    if (name.contains(QLatin1String(".."))) {
        return Response{404, "text/plain", "Not Found", false};
    }

    const QString full = config.assetRoot + QLatin1Char('/') + name;
    QFile file(full);

    if (!file.open(QIODevice::ReadOnly)) {
        // Unknown path -> index.html, so the React router owns deep links.
        QFile fallback(config.assetRoot + QStringLiteral("/index.html"));
        if (!fallback.open(QIODevice::ReadOnly)) {
            return Response{404, "text/plain", "UI bundle not found. Run: cd ui && npm run build",
                            false};
        }
        Response r;
        r.contentType = "text/html; charset=utf-8";
        r.body = fallback.readAll().replace("%%AT_BASE%%", basePath().toUtf8());
        return r;
    }

    Response r;
    r.contentType = mimeForPath(name);
    r.body = file.readAll();

    // THE SUBSTITUTION (AD-08). index.html carries the placeholder exactly once,
    // and it is spelled differently from the window.__AT_BASE__ global so this
    // replace cannot corrupt the property name. See ui/index.html.
    if (name.endsWith(QLatin1String(".html"))) {
        r.body.replace("%%AT_BASE%%", basePath().toUtf8());
    }
    return r;
}

void LocalHttpServer::Impl::respond(QTcpSocket* socket, const Response& response)
{
    socket->write(response.serialise());
    socket->flush();
    socket->disconnectFromHost();
}

}  // namespace anytime::http
