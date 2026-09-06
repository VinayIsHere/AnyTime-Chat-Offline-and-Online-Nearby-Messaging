// adapters/http/LocalHttpServer.h
//
// Hand-rolled HTTP/1.1 on QTcpServer (AD-03).
//
// WHY NOT QHttpServer: it ships under GPLv3 or a commercial licence with no
// LGPL option, so linking it would relicense the entire application. Qt Network
// is LGPL. FunMeet hand-rolled the same thing — this is the one place we copy it
// directly rather than improving on it.
//
// SECURITY (AD-08, §10): Android's loopback is shared by every app on the device,
// so any installed app can probe 127.0.0.1. Four measures together close it:
//
//   1. bind 127.0.0.1 with port 0        — ephemeral port, no fixed target
//   2. secret 256-bit path prefix        — the real gate; every route lives
//                                          under /{token}/, everything else 404s
//   3. Host and Origin validation        — defeats DNS rebinding
//   4. no CORS headers at all            — a hostile page cannot read responses
//
// The prefix beats a bearer token because it is RACE-FREE: C++ generates it,
// builds the URL and hands it to the WebView, so there is no window in which the
// page has loaded but not yet received its credential.

#pragma once

#include <QObject>
#include <QString>

#include <memory>

namespace anytime {
class AppCore;
}

namespace anytime::http {

class Router;
class SseHub;

struct ServerConfig {
    /// Directory served at the prefix root. On Android this is "assets:/www".
    QString assetRoot = QStringLiteral("assets:/www");

    /// 0 asks the OS for an ephemeral port. Never hard-code one.
    quint16 port = 0;

    /// Rejects any request whose socket peer is not loopback. Three lines, and
    /// it closes the case where a future change binds a wider interface.
    bool requireLoopbackPeer = true;
};

class LocalHttpServer : public QObject {
    Q_OBJECT

public:
    /// `sse` is the SAME hub the core was given as its IEventSink. It is passed
    /// in rather than owned here because owning one would create a second,
    /// unrelated hub: the core would publish into main()'s instance while
    /// browsers attached to this one, and no event would ever be delivered.
    LocalHttpServer(AppCore&, SseHub& sse, ServerConfig, QObject* parent = nullptr);
    ~LocalHttpServer() override;

    /// Binds and starts listening. Generates the secret prefix.
    bool start();
    void stop();

    quint16 port() const;

    /// The 64-hex-character secret path segment, generated per launch.
    QString token() const;

    /// The full URL to hand the WebView: http://127.0.0.1:<port>/<token>/
    /// Relative URLs inside the page inherit the prefix automatically, so the
    /// React client needs only its base URL changed.
    QString rootUrl() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace anytime::http
