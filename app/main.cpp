// app/main.cpp
//
// The composition root. This is the ONLY file that knows which concrete adapter
// backs each port — everything else depends on interfaces.
//
// Read this file to understand the wiring; read docs/ARCHITECTURE.md section 04
// to understand why it is shaped this way.

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QStandardPaths>
#include <QTextStream>

#include <cstdio>
#include <cstdlib>

#ifdef ANYTIME_USE_WEBVIEW
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QTimer>
#include <QtWebView>
#endif

#include <memory>

#include "adapters/clock/QtClock.h"
#include "adapters/codec/QtCborCodec.h"
#include "adapters/crypto/NoiseSessionFactory.h"
#include "adapters/http/LocalHttpServer.h"
#include "adapters/http/SseHub.h"
#include "adapters/identity/MonocypherIdentityStore.h"
#include "adapters/logging/LogSetup.h"
#include "adapters/persistence/settings/SettingsStore.h"
#include "adapters/persistence/sqlite/MonocypherDataCipher.h"
#include "adapters/persistence/sqlite/SqliteStore.h"
#include "adapters/transport/loopback/LoopbackTransport.h"
#include "core/log/Log.h"
#include "core/protocol/Packet.h"
#ifdef Q_OS_ANDROID
#include "adapters/events/FanOutEventSink.h"
#include "adapters/notify/AndroidNotificationPresenter.h"
#include "adapters/notify/Notifier.h"
#include "adapters/radio/AndroidRadioController.h"
#include "adapters/transport/ble/BleGattTransport.h"
#include "adapters/transport/nearby/NearbyConnectionsTransport.h"
#endif
#include "core/AppCore.h"

using namespace anytime;

namespace {

#ifdef Q_OS_ANDROID
/// How long the launch splash takes to fade once the web UI is up. Short: the
/// web splash underneath shows the same logo in the same place, so this is a
/// cross-fade between two near-identical frames, not a transition to watch.
constexpr int kSplashFadeMillis = 200;
#endif

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

/// One-time import of the account SettingsStore used to own.
///
/// SettingsStore kept the account in an INI file; SQLite owns it now. Without
/// this, updating the app would be indistinguishable from being signed out —
/// and because the PeerId is derived from the identity key rather than the
/// account row, the user would keep their key but lose the profile attached to
/// it. The INI is left in place rather than deleted, so a downgrade still works.
void migrateLegacyAccount(const QString& iniPath, persistence::SqliteStore& store)
{
    if (!QFile::exists(iniPath)) return;

    auto existing = store.load();
    if (existing.isError() || existing.value().has_value()) return;  // already migrated

    persistence::SettingsStore legacy(iniPath);
    auto account = legacy.load();
    if (account.isError() || !account.value().has_value()) return;

    if (const Result r = store.save(*account.value()); r.isError()) {
        AT_LOG_ERROR("app", "could not migrate the legacy account: {}", r.message());
        return;
    }
    AT_LOG_INFO("app", "migrated the account from settings storage into SQLite");
}

#ifndef ANYTIME_USE_WEBVIEW
/// Printed straight to stdout, NOT through the logger. The URL is the only way
/// into the headless desktop app, and a Release build logs at critical only —
/// routing this through spdlog would hide it from anyone running a release
/// binary. It also keeps the secret token out of the log file.
void printBanner(const QString& instance, const QString& url, const QString& urlFile,
                 const QString& logFile)
{
    const auto line = [](const QString& text) {
        std::fputs(("  |  " + text + QLatin1Char('\n')).toLocal8Bit().constData(), stdout);
    };

    std::fputs("\n  +--------------------------------------------------------------\n", stdout);
    line(QStringLiteral("AnyTime Chat %1%2")
             .arg(QStringLiteral(ANYTIME_VERSION),
                  instance.isEmpty() ? QString() : QStringLiteral(" [instance %1]").arg(instance)));
    line(QString());
    line(QStringLiteral("Open this in a browser:"));
    line(url);
    line(QString());
    line(QStringLiteral("Second instance:  anytime --instance b"));
    line(QString());
    line(QStringLiteral("URL also written to:"));
    line(urlFile);
    if (!logFile.isEmpty()) {
        line(QString());
        line(QStringLiteral("Log file (level: %1):")
                 .arg(QString::fromStdString(logging::compiledLevelName())));
        line(logFile);
    }
    std::fputs("  +--------------------------------------------------------------\n\n", stdout);
    std::fflush(stdout);
}
#endif

/// Everything the app does, start to finish. Separate from main() so that every
/// object below is destroyed — the database closed, the log flushed — BEFORE
/// main() decides how the process leaves. See main().
int runApp(int argc, char* argv[])
{
    // First, so nothing Qt prints during start-up bypasses the logger. Until
    // initialise() runs below, spdlog's default console logger catches it.
    logging::installQtMessageBridge();

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

    // ── Logging ──────────────────────────────────────────────────────────────
    // Console (logcat on Android) plus a rotating file under the data directory.
    // The level is fixed at compile time — see ANYTIME_RELEASE_LOG_LEVEL in
    // CMakeLists.txt.

    logging::LogConfig logConfig;
    logConfig.directory = dataDir + QStringLiteral("/logs");
    logging::initialise(logConfig);

    AT_LOG_INFO("app", "AnyTime Chat {} starting{}", ANYTIME_VERSION,
                instance.isEmpty() ? std::string() : " [instance " + instance.toStdString() + "]");
    AT_LOG_DEBUG("app", "data directory: {}", dataDir.toStdString());

    // ── Adapters ─────────────────────────────────────────────────────────────
    // main() owns these; AppCore borrows them and never outlives them.

    platform::QtClock clock;

    // Real Ed25519 signatures and X25519 keys, via vendored Monocypher.
    // Secret keys are still plaintext on disk — Keystore wrapping is outstanding.
    identity::MonocypherIdentityStore identityStore(dataDir + QStringLiteral("/identity.key"));

    // Durable storage (section 12): WAL, forward-only migrations, and message
    // bodies plus outbox payloads encrypted at rest. This replaced SettingsStore,
    // which kept everything but the account in RAM — chat history that vanished
    // on restart is not chat history.
    persistence::MonocypherDataCipher dataCipher(dataDir + QStringLiteral("/data.key"));
    if (const Result r = dataCipher.loadOrCreate(); r.isError()) {
        // Refusing to start beats starting with unreadable history: carrying on
        // would silently render every stored message blank.
        AT_LOG_CRITICAL("app", "at-rest data key unavailable: {}", r.message());
        logging::shutdown();
        return 1;
    }

    persistence::SqliteStore store(dataDir + QStringLiteral("/anytime.db"), dataCipher, clock);
    if (const Result r = store.open(); r.isError()) {
        AT_LOG_CRITICAL("app", "database unavailable: {}", r.message());
        logging::shutdown();
        return 1;
    }

    migrateLegacyAccount(dataDir + QStringLiteral("/anytime.ini"), store);

    codec::QtCborCodec          cbor;
    crypto::NoiseSessionFactory sessionFactory(identityStore);
    http::SseHub                sseHub;

#ifdef Q_OS_ANDROID
    // Switching the radio ON, which is a separate concern from carrying bytes
    // over it — see core/ports/IRadioController.h. Desktop leaves deps.radio
    // null on purpose: there is no radio to switch, and the API then reports
    // "unsupported" so the UI knows not to gate anything.
    radio::AndroidRadioController radioController;

    // Notifications for someone who has left the app. The core still emits each
    // event once; the fan-out hands it to the page (SSE) AND to the notifier.
    // Desktop has no notifier: the app is the window, and a developer running
    // two instances side by side does not want each pinging the other.
    notify::AndroidNotificationPresenter notificationPresenter;
    notify::Notifier                     notifier(store, store, notificationPresenter);
    events::FanOutEventSink              eventSink{&sseHub, &notifier};
#endif

    // ── Core ─────────────────────────────────────────────────────────────────

    AppCoreDeps deps;
    deps.accounts = &store;
    deps.contacts = &store;
    deps.messages = &store;
    deps.outbox   = &store;
    deps.transactions = &store;
    deps.identity = &identityStore;
    deps.sessions = &sessionFactory;
    deps.cbor     = &cbor;
#ifdef Q_OS_ANDROID
    deps.events   = &eventSink;  // -> SSE frames for the page, and notifications
#else
    deps.events   = &sseHub;     // core emits domain events -> SSE frames
#endif
    deps.clock    = &clock;
#ifdef Q_OS_ANDROID
    deps.radio    = &radioController;
#endif

    AppCore core(deps);

    // ── Transports ───────────────────────────────────────────────────────────
    // AD-07: loopback is a shipped adapter, not a test double. It is the only
    // transport on desktop, because we deliberately never write a desktop radio.

#ifdef Q_OS_ANDROID
    // The real radio. BleRadio.java owns the GATT plumbing; everything about
    // what the bytes MEAN stays in core/ (section 11).
    transport::BleConfig bleConfig;
    bleConfig.serviceUuid = QString::fromLatin1(protocol::kServiceUuid);
    bleConfig.frameSize = protocol::kFragmentPayloadBytes;
    core.addTransport(std::make_unique<transport::BleGattTransport>(bleConfig));

    // ── Nearby Connections: the BULK transport, registered CONDITIONALLY ─────
    //
    // Registered only when Play Services is actually present, and that ordering
    // is load-bearing rather than tidy. TransportRegistry::allAvailable() is
    // true only when EVERY registered transport reports itself usable, and
    // AppCore retries transports every kTransportRetryMillis until it is. So
    // registering a permanently dead Nearby on a de-Googled phone would spin
    // that retry every three seconds for the life of the process — a battery
    // drain caused entirely by bookkeeping.
    //
    // Skipping registration instead means such a device simply has one
    // transport, RoutingPolicy never offers Bulk anywhere else, and photos fall
    // back to BLE: slower, but working. BLE remains primary either way.
    if (transport::NearbyConnectionsTransport::isSupportedOnThisDevice()) {
        transport::NearbyConfig nearbyConfig;
        nearbyConfig.serviceId = QString::fromLatin1(protocol::kServiceUuid);
        core.addTransport(
            std::make_unique<transport::NearbyConnectionsTransport>(nearbyConfig));
        AT_LOG_INFO("app", "Nearby Connections registered as the bulk transport");
    } else {
        AT_LOG_INFO("app",
                    "Play Services absent; photos will travel over BLE instead of Nearby");
    }
#else
    // Desktop has no radio by design — we never write one (AD-07). Two instances
    // discover each other over local sockets instead.
    transport::LoopbackConfig loopbackConfig;
    loopbackConfig.rendezvousDir = QDir::tempPath() + QStringLiteral("/anytime-loopback");
    core.addTransport(std::make_unique<transport::LoopbackTransport>(loopbackConfig));
#endif

    // A failed start is survivable: the shell still serves the UI, and AppCore
    // keeps retrying transports on its own.
    if (const Result r = core.start(); r.isError()) {
        AT_LOG_ERROR("app", "core start failed: {}", r.message());
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
        // Without the server there is no UI at all, so this is the one start-up
        // failure worth exiting over.
        AT_LOG_CRITICAL("app", "local API server failed to bind; exiting");
        core.stop();
        logging::shutdown();
        return 1;
    }

    // Also written to a file: the URL contains a fresh 64-hex token every launch,
    // so without this there is no way to reach the app except by reading the
    // console at exactly the right moment. The URL itself is never logged.
    const QString urlFile = dataDir + QStringLiteral("/current-url.txt");
    if (QFile f(urlFile); f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QTextStream(&f) << server.rootUrl() << '\n';
    } else {
        AT_LOG_WARN("app", "could not write {}", urlFile.toStdString());
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
                     []() {
                         AT_LOG_CRITICAL("app", "the WebView shell failed to load");
                         QCoreApplication::exit(-1);
                     },
                     Qt::QueuedConnection);

    engine.loadFromModule("AnyTime", "MainWebView");
    AT_LOG_DEBUG("app", "WebView shell loading");

#ifdef Q_OS_ANDROID
    // ── Launch splash ────────────────────────────────────────────────────────
    // The manifest marks Qt's splash "sticky", so it stays up until told
    // otherwise — and this is where it is told, once MainWebView.qml reports the
    // page has loaded. The web splash is on screen underneath by then, with the
    // logo in the same place, so the fade reads as the logo coming to life
    // rather than one screen replacing another.
    //
    // The timer is only an adaptor: a QML signal can be connected by name to a
    // slot but not to a lambda, and QTimer::start() is a slot that already
    // exists. Hiding twice is harmless, so the signal firing again on a later
    // page load needs no guard.
    QTimer splashLift;
    splashLift.setSingleShot(true);
    splashLift.setInterval(0);
    QObject::connect(&splashLift, &QTimer::timeout, &app, [] {
        AT_LOG_DEBUG("app", "launch splash lifted");
        QNativeInterface::QAndroidApplication::hideSplashScreen(kSplashFadeMillis);
    });
    if (engine.rootObjects().isEmpty()) {
        splashLift.start();  // no shell, so nothing will ever report ready
    } else {
        QObject::connect(engine.rootObjects().constFirst(), SIGNAL(uiReady()),
                         &splashLift, SLOT(start()));
    }
#endif
#else
    // Desktop: headless. Open the URL in a browser. Run a second copy with
    // --instance b and the two discover each other over loopback (AD-07).
    printBanner(instance, server.rootUrl(), urlFile, logging::currentLogFile());
#endif

    const int rc = app.exec();
    AT_LOG_INFO("app", "event loop exited with code {}", rc);

    core.stop();
    server.stop();
    logging::shutdown();
    return rc;
}

}  // namespace

int main(int argc, char* argv[])
{
    const int rc = runApp(argc, argv);

#ifdef Q_OS_ANDROID
    // LEAVE WITHOUT RUNNING STATIC DESTRUCTORS.
    //
    // Returning from main() hands control back to Qt's Android plugin, which
    // calls exit(). exit() runs every global destructor in the process while
    // the platform's own threads are still alive — and on Android 16 libgui's
    // fence-monitor threads ("HWC release" and friends) are then waiting on
    // mutexes that have just been destroyed:
    //
    //     FORTIFY: pthread_mutex_lock called on a destroyed mutex
    //
    // which aborts the process. The app had already shut down cleanly by then,
    // so the user lost nothing, but Android recorded a native crash every time
    // the app was swiped away.
    //
    // _Exit ends the process without that step. Nothing is skipped that
    // matters: runApp() has returned, so all of our own objects are already
    // destroyed in order, and the kernel reclaims the rest.
    std::_Exit(rc);
#else
    return rc;
#endif
}
