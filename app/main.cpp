// app/main.cpp
//
// The composition root. This is the ONLY file that knows which concrete adapter
// backs each port — everything else depends on interfaces.
//
// Read this file to understand the wiring; read docs/ARCHITECTURE.md section 04
// to understand why it is shaped this way.
//
// MILESTONE STATE: sign-in and Nearby are implemented end to end. Messaging
// still needs SQLite, real crypto and the BLE radio — the adapters marked below
// are the ones that get replaced, and nothing above them changes.

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QStandardPaths>
#include <QTextStream>

#include <cstdio>

#ifdef ANYTIME_USE_WEBVIEW
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QtWebView>
#endif

#include <memory>

#include "adapters/clock/QtClock.h"
#include "adapters/codec/QtCborCodec.h"
#include "adapters/crypto/NullSessionFactory.h"
#include "adapters/http/LocalHttpServer.h"
#include "adapters/http/SseHub.h"
#include "adapters/identity/DevIdentityStore.h"
#include "adapters/persistence/settings/SettingsStore.h"
#include "adapters/transport/loopback/LoopbackTransport.h"
#include "core/AppCore.h"

using namespace anytime;

namespace {

/// Per-instance data directory. `--instance b` gives a second, independent
/// identity on the same machine, which is how two windows discover each other
/// over the loopback transport (AD-07).
QString appDataDir(const QString& instance)
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (!instance.isEmpty()) dir += QStringLiteral("/instance-") + instance;
    QDir().mkpath(dir);
    return dir;
}

/// Qt's default handler on Windows routes to OutputDebugString when there is no
/// attached console, so a redirected `> log.txt` captures nothing. Since the
/// desktop build prints the URL you need in order to use the app at all, that
/// silence is a usability bug rather than a cosmetic one.
void writeToStdout(QtMsgType type, const QMessageLogContext&, const QString& message)
{
    const char* prefix = "";
    switch (type) {
        case QtWarningMsg:  prefix = "[warn] "; break;
        case QtCriticalMsg: prefix = "[error] "; break;
        case QtFatalMsg:    prefix = "[fatal] "; break;
        default: break;
    }
    std::fputs(prefix, stdout);
    std::fputs(qPrintable(message), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

}  // namespace

int main(int argc, char* argv[])
{
    qInstallMessageHandler(writeToStdout);

#ifdef ANYTIME_USE_WEBVIEW
    QtWebView::initialize();
#endif

    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("AnyTime Chat"));
    app.setOrganizationName(QStringLiteral("AnyTime"));
    app.setApplicationVersion(QStringLiteral(ANYTIME_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("AnyTime Chat"));
    parser.addHelpOption();
    QCommandLineOption instanceOption(
        QStringLiteral("instance"),
        QStringLiteral("Separate identity and storage, for running two copies side by side."),
        QStringLiteral("name"));
    parser.addOption(instanceOption);
    parser.process(app);

    const QString instance = parser.value(instanceOption);
    const QString dataDir = appDataDir(instance);

    // ── Adapters ─────────────────────────────────────────────────────────────
    // main() owns these; AppCore borrows them and never outlives them.

    platform::QtClock clock;

    // TEMPORARY (see the header): not real cryptography. Replaced by the
    // libsodium-backed store in the crypto milestone.
    identity::DevIdentityStore identityStore(dataDir + QStringLiteral("/identity.key"));

    // TEMPORARY: account persists, peers are RAM-only. SQLite lands with messaging.
    persistence::SettingsStore store(dataDir + QStringLiteral("/anytime.ini"));

    codec::QtCborCodec        cbor;
    crypto::NullSessionFactory sessionFactory;
    http::SseHub               sseHub;

    // ── Core ─────────────────────────────────────────────────────────────────

    AppCoreDeps deps;
    deps.accounts = &store;
    deps.contacts = &store;
    deps.messages = nullptr;   // messaging milestone
    deps.outbox   = nullptr;   // messaging milestone
    deps.identity = &identityStore;
    deps.sessions = &sessionFactory;
    deps.cbor     = &cbor;
    deps.events   = &sseHub;   // core emits domain events -> SSE frames
    deps.clock    = &clock;

    AppCore core(deps);

    // ── Transports ───────────────────────────────────────────────────────────
    // AD-07: loopback is a shipped adapter, not a test double. It is the only
    // transport on desktop, because we deliberately never write a desktop radio.

    transport::LoopbackConfig loopbackConfig;
    loopbackConfig.rendezvousDir = QDir::tempPath() + QStringLiteral("/anytime-loopback");
    core.addTransport(std::make_unique<transport::LoopbackTransport>(loopbackConfig));

    if (const Result r = core.start(); r.isError()) {
        qWarning("[AnyTime] core start: %s", r.message().c_str());
    }

    // ── Local API server  (AD-03, AD-08) ─────────────────────────────────────
    // Ephemeral port + 256-bit secret path prefix. The WebView is handed the
    // full URL including the prefix, so there is no injection race.

    http::ServerConfig serverConfig;
#ifdef Q_OS_ANDROID
    serverConfig.assetRoot = QStringLiteral("assets:/www");
#else
    serverConfig.assetRoot = QStringLiteral(ANYTIME_UI_DIST_DIR);
#endif

    http::LocalHttpServer server(core, sseHub, serverConfig);
    if (!server.start()) {
        qFatal("[AnyTime] local API server failed to bind");
    }
    // Also written to a file: the URL contains a fresh 64-hex token every launch,
    // so without this there is no way to reach the app except by reading the
    // console at exactly the right moment.
    const QString urlFile = dataDir + QStringLiteral("/current-url.txt");
    if (QFile f(urlFile); f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QTextStream(&f) << server.rootUrl() << '\n';
    }

    // ── UI shell ─────────────────────────────────────────────────────────────

#ifdef ANYTIME_USE_WEBVIEW
    // Mobile: the platform WebView loads the URL, secret prefix and all.
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("appRootUrl"), server.rootUrl());
    engine.rootContext()->setContextProperty(QStringLiteral("appVersion"),
                                             app.applicationVersion());
    engine.rootContext()->setContextProperty(QStringLiteral("appInstance"), instance);

    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                     []() { QCoreApplication::exit(-1); }, Qt::QueuedConnection);

    engine.loadFromModule("AnyTime", "MainWebView");
#else
    // Desktop: headless. Open the URL below in a browser. Run a second copy with
    // --instance b and the two discover each other over loopback (AD-07).
    qInfo("");
    qInfo("  ┌──────────────────────────────────────────────────────────────");
    qInfo("  │  AnyTime Chat %s%s", ANYTIME_VERSION,
          instance.isEmpty() ? "" : qPrintable(QStringLiteral(" [instance %1]").arg(instance)));
    qInfo("  │");
    qInfo("  │  Open this in a browser:");
    qInfo("  │  %s", qPrintable(server.rootUrl()));
    qInfo("  │");
    qInfo("  │  Second instance:  anytime --instance b");
    qInfo("  │");
    qInfo("  │  URL also written to:");
    qInfo("  │  %s", qPrintable(urlFile));
    qInfo("  └──────────────────────────────────────────────────────────────");
    qInfo("");
#endif

    const int rc = app.exec();

    core.stop();
    server.stop();
    return rc;
}
