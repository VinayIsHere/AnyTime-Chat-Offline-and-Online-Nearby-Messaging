// adapters/transport/ble/BleGattTransport.cpp
//
// JNI binding onto BleRadio.java. Read the header first for the dumb-pipe rule.
//
// THE THREAD HOP IS THE POINT OF THIS FILE. Android delivers every BLE callback
// on a binder thread; the C++ core is single-threaded by design. Each native
// callback below therefore does nothing but marshal its arguments and post them
// to the transport's own thread via a queued invocation. Nothing here touches
// core state directly.
//
// Skip that and you get intermittent corruption under concurrent links — the
// hardest class of bug to reproduce, and it presents as a radio problem rather
// than a threading one.

#include "adapters/transport/ble/BleGattTransport.h"

#include <QCoreApplication>
#include <QJniEnvironment>
#include <QJniObject>
#include <QMetaObject>
#include <QPointer>

#include <cstring>

#include "core/domain/Types.h"
#include "core/log/Log.h"

namespace anytime::transport {

namespace {

constexpr const char* kRadioClass      = "com/anytimechat/ble/BleRadio";
constexpr const char* kPermissionClass = "com/anytimechat/PermissionBridge";
constexpr const char* kKeepAliveClass  = "com/anytimechat/MeshForegroundService";

/// The single live transport, so the static JNI callbacks know where to post.
/// One BLE radio per process, so a single pointer is honest rather than limiting.
QPointer<BleGattTransport> g_transport;

/// start() is retried every few seconds while the radio is unavailable, so both
/// of these one-time actions need remembering across attempts.
bool g_nativesRegistered   = false;
/// Re-prompting on every retry would put the permission dialog straight back in
/// front of someone who just dismissed it.
bool g_permissionsRequested = false;

PeerId peerIdFrom(JNIEnv* env, jbyteArray array)
{
    PeerIdBytes bytes{};
    if (array == nullptr) return PeerId{bytes};

    const jsize length = env->GetArrayLength(array);
    if (length < static_cast<jsize>(kPeerIdBytes)) return PeerId{bytes};

    env->GetByteArrayRegion(array, 0, static_cast<jsize>(kPeerIdBytes),
                            reinterpret_cast<jbyte*>(bytes.data()));
    return PeerId{bytes};
}

QByteArray bytesFrom(JNIEnv* env, jbyteArray array)
{
    if (array == nullptr) return {};
    const jsize length = env->GetArrayLength(array);

    QByteArray out(length, Qt::Uninitialized);
    env->GetByteArrayRegion(array, 0, length, reinterpret_cast<jbyte*>(out.data()));
    return out;
}

/// The foreground service keeps the process — and with it the whole C++ mesh —
/// alive once the user leaves the app. Started only after the radio is up:
/// Android 14 refuses a connectedDevice foreground service until the Bluetooth
/// permissions are granted, and a permanent notification for a radio that is
/// not running would be a lie.
void setKeepAlive(const QJniObject& context, bool on)
{
    if (!context.isValid()) return;
    QJniObject::callStaticMethod<void>(kKeepAliveClass, on ? "start" : "stop",
                                       "(Landroid/content/Context;)V", context.object());
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────

struct BleGattTransport::Impl {
    BleConfig           config;
    ITransportListener* listener = nullptr;
    QJniObject          radio;

    bool started     = false;
    bool available   = false;
    bool advertising = false;
    bool discovering = false;

    /// Peer ids learned from advertisements, keyed by device address. A link
    /// comes up before the GATT layer knows who is on the other end, so the
    /// address is the only thing that ties the two together.
    QHash<QString, PeerId> peerByAddress;
    QHash<LinkId, PeerId>  peerByLink;
};

// ─────────────────────────────────────────────────────────────────────────────

BleGattTransport::BleGattTransport(BleConfig config, QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>())
{
    impl_->config = std::move(config);
    g_transport = this;
}

BleGattTransport::~BleGattTransport()
{
    stop();
    if (g_transport == this) g_transport = nullptr;
}

TransportCapabilities BleGattTransport::caps() const
{
    TransportCapabilities caps;
    // Fixed and conservative. We never query MTU to size protocol frames
    // (section 11) — BleRadio chunks to whatever the link negotiated, which is a
    // separate concern from what the core is allowed to build.
    caps.maxFramePayload = impl_->config.frameSize;
    caps.throughputClass = 0;
    caps.supportsBulk = false;
    caps.supportsMesh = true;
    caps.requiresUserConsent = false;
    caps.energyCost = EnergyCost::Low;
    return caps;
}

bool BleGattTransport::isAvailable() const { return impl_->available; }

void BleGattTransport::setListener(ITransportListener* listener) { impl_->listener = listener; }

Result BleGattTransport::start(const PeerId& localPeerId)
{
    if (impl_->started && impl_->available) return Result::ok();

    QJniEnvironment env;
    if (!env.isValid()) {
        return Result::error(ErrorCode::TransportUnavailable, "No JNI environment.");
    }

    // Before BleRadio is touched: an unregistered native method aborts the
    // process on its first callback rather than failing politely.
    if (!registerNatives()) {
        AT_LOG_ERROR("ble", "could not register the BLE native callbacks");
        return Result::error(ErrorCode::TransportUnavailable,
                             "Could not register BLE native methods.");
    }

    const QJniObject context = QNativeInterface::QAndroidApplication::context();
    if (!context.isValid()) {
        return Result::error(ErrorCode::TransportUnavailable, "No Android context.");
    }

    // Permissions BEFORE opening the GATT server. Without BLUETOOTH_SCAN on API
    // 31+ scanning does not fail — it silently returns nothing, which looks
    // exactly like "no one is nearby" and is the most confusing way for this to
    // break.
    if (!ensurePermissions()) {
        AT_LOG_DEBUG("ble", "waiting for Bluetooth permission");
        return Result::error(ErrorCode::PermissionDenied,
                             "Bluetooth permission has not been granted yet.");
    }

    if (!impl_->radio.isValid()) {
        impl_->radio = QJniObject(kRadioClass, "(Landroid/content/Context;)V", context.object());
        if (!impl_->radio.isValid()) {
            AT_LOG_ERROR("ble", "could not construct BleRadio");
            return Result::error(ErrorCode::TransportUnavailable,
                                 "Could not construct BleRadio.");
        }
    }

    // Started once and then lost the radio (Bluetooth switched off). Tear the
    // Java side down fully first, so the GATT server is never opened twice.
    if (impl_->started) {
        impl_->radio.callMethod<void>("stop");
        impl_->started = false;
    }

    QJniObject uuid = QJniObject::fromString(impl_->config.serviceUuid);

    jbyteArray peerBytes = env->NewByteArray(static_cast<jsize>(kPeerIdBytes));
    env->SetByteArrayRegion(peerBytes, 0, static_cast<jsize>(kPeerIdBytes),
                            reinterpret_cast<const jbyte*>(localPeerId.bytes().data()));

    const jboolean ok = impl_->radio.callMethod<jboolean>(
        "start", "(Ljava/lang/String;[B)Z", uuid.object<jstring>(), peerBytes);
    env->DeleteLocalRef(peerBytes);

    if (!ok) {
        AT_LOG_DEBUG("ble", "radio did not start: Bluetooth is off or unsupported");
        return Result::error(ErrorCode::TransportUnavailable, "Bluetooth is switched off.");
    }

    impl_->started = true;

    // Marked available here rather than on a callback. start() returning true
    // means the adapter is on and the GATT server is open, and a queued
    // radio-state event would arrive after the setAdvertising() call that
    // immediately follows — which would then see a dead transport and do nothing.
    impl_->available = true;

    setKeepAlive(context, true);
    AT_LOG_INFO("ble", "radio started");
    return Result::ok();
}

void BleGattTransport::stop()
{
    if (!impl_->started) return;
    if (impl_->radio.isValid()) impl_->radio.callMethod<void>("stop");

    setKeepAlive(QNativeInterface::QAndroidApplication::context(), false);

    impl_->started = false;
    impl_->available = false;
    impl_->advertising = false;
    impl_->discovering = false;
    impl_->peerByAddress.clear();
    impl_->peerByLink.clear();

    AT_LOG_INFO("ble", "radio stopped");
}

Result BleGattTransport::startDiscovery(DiscoveryMode mode)
{
    if (!impl_->started) {
        return Result::error(ErrorCode::TransportUnavailable, "Radio not started.");
    }
    if (mode == DiscoveryMode::Off) {
        stopDiscovery();
        return Result::ok();
    }

    // THE DUTY CYCLE IS A CORE DECISION, NOT A RADIO ONE. Active means the user is
    // watching the Nearby screen and wants results now; Passive is the background
    // state that keeps the mesh reachable without scanning continuously. Mapping
    // onto Android's ScanSettings constants is all the radio is allowed to decide.
    //   SCAN_MODE_LOW_POWER = 0   (~10% duty cycle)
    //   SCAN_MODE_LOW_LATENCY = 2 (continuous)
    const jint androidScanMode = (mode == DiscoveryMode::Active) ? 2 : 0;

    impl_->radio.callMethod<void>("startScanning", "(I)V", androidScanMode);
    impl_->discovering = true;

    AT_LOG_DEBUG("ble", "scanning in {} mode",
                 mode == DiscoveryMode::Active ? "active" : "passive");
    return Result::ok();
}

void BleGattTransport::stopDiscovery()
{
    if (impl_->radio.isValid()) impl_->radio.callMethod<void>("stopScanning");
    impl_->discovering = false;
}

Result BleGattTransport::startAdvertising()
{
    if (!impl_->started) {
        return Result::error(ErrorCode::TransportUnavailable, "Radio not started.");
    }
    impl_->radio.callMethod<void>("startAdvertising");
    impl_->advertising = true;
    return Result::ok();
}

void BleGattTransport::stopAdvertising()
{
    if (impl_->radio.isValid()) impl_->radio.callMethod<void>("stopAdvertising");
    impl_->advertising = false;
}

Outcome<LinkId> BleGattTransport::openLink(const PeerHandle& handle)
{
    if (!impl_->started) {
        return Outcome<LinkId>::error(ErrorCode::TransportUnavailable, "Radio not started.");
    }

    const QString address = QString::fromStdString(handle.address);
    QJniObject addressObject = QJniObject::fromString(address);

    const jint linkId = impl_->radio.callMethod<jint>("openLink", "(Ljava/lang/String;)I",
                                                      addressObject.object<jstring>());
    if (linkId == 0) {
        AT_LOG_DEBUG("ble", "GATT connection to {} could not be started", handle.peerId.toHex());
        return Outcome<LinkId>::error(ErrorCode::LinkClosed, "Could not start a GATT connection.");
    }

    // Remember who this address belongs to: the GATT layer reports a link up
    // before it knows the peer id, and the advert is where that came from.
    impl_->peerByAddress.insert(address, handle.peerId);
    impl_->peerByLink.insert(static_cast<LinkId>(linkId), handle.peerId);

    return static_cast<LinkId>(linkId);
}

void BleGattTransport::closeLink(LinkId link)
{
    if (impl_->radio.isValid()) {
        impl_->radio.callMethod<void>("closeLink", "(I)V", static_cast<jint>(link));
    }
    impl_->peerByLink.remove(link);
}

Result BleGattTransport::send(LinkId link, ByteSpan frame)
{
    if (!impl_->started) {
        return Result::error(ErrorCode::TransportUnavailable, "Radio not started.");
    }
    if (frame.size() > impl_->config.frameSize) {
        return Result::error(ErrorCode::InvalidArgument, "Frame exceeds the transport budget.");
    }

    // kInvalidLink means broadcast — how announces reach every open link.
    if (link == kInvalidLink) {
        bool anySent = false;
        const QList<LinkId> links = impl_->peerByLink.keys();
        for (LinkId each : links) {
            if (send(each, frame).isOk()) anySent = true;
        }
        return anySent ? Result::ok()
                       : Result::error(ErrorCode::LinkClosed, "No open links to broadcast on.");
    }

    QJniEnvironment env;
    jbyteArray array = env->NewByteArray(static_cast<jsize>(frame.size()));
    env->SetByteArrayRegion(array, 0, static_cast<jsize>(frame.size()),
                            reinterpret_cast<const jbyte*>(frame.data()));

    const jboolean ok = impl_->radio.callMethod<jboolean>("send", "(I[B)Z",
                                                          static_cast<jint>(link), array);
    env->DeleteLocalRef(array);

    if (!ok) {
        AT_LOG_DEBUG("ble", "link {} refused a {}-byte frame", link, frame.size());
        return Result::error(ErrorCode::SendFailed, "GATT write refused.");
    }
    return Result::ok();
}

// ── invoked on the CORE thread, posted from the binder-thread callbacks ──────

void BleGattTransport::deliverPeerSeen(const PeerId& peerId, const QString& address, int rssi)
{
    impl_->peerByAddress.insert(address, peerId);

    if (!impl_->listener) return;

    PeerHandle handle;
    handle.peerId = peerId;
    handle.address = address.toStdString();
    handle.rssi = rssi;
    impl_->listener->onPeerSeen(id(), handle);
}

void BleGattTransport::deliverLinkUp(LinkId link, const PeerId& peerIdFromJava)
{
    // An incoming connection reports an all-zero peer id, because a peripheral
    // learns nothing about the central until it speaks. The ANNOUNCE that
    // follows is what establishes identity (TransportRegistry::bindLink).
    PeerId peerId = peerIdFromJava;
    if (peerId.isNull()) peerId = impl_->peerByLink.value(link, PeerId{});

    impl_->peerByLink.insert(link, peerId);
    if (impl_->listener) impl_->listener->onLinkUp(id(), link, peerId);
}

void BleGattTransport::deliverLinkDown(LinkId link, int reason)
{
    impl_->peerByLink.remove(link);

    static const LinkCloseReason kReasons[] = {LinkCloseReason::Local, LinkCloseReason::Remote,
                                               LinkCloseReason::Timeout, LinkCloseReason::Error};
    const LinkCloseReason mapped =
        (reason >= 0 && reason < 4) ? kReasons[reason] : LinkCloseReason::Error;

    if (impl_->listener) impl_->listener->onLinkDown(id(), link, mapped);
}

void BleGattTransport::deliverFrame(LinkId link, const QByteArray& frame)
{
    if (!impl_->listener) return;
    impl_->listener->onFrame(id(), link,
                             ByteSpan{reinterpret_cast<const Byte*>(frame.constData()),
                                      static_cast<std::size_t>(frame.size())});
}

void BleGattTransport::deliverRadioState(bool available)
{
    if (!impl_->started) return;  // a late event from a radio we already stopped
    if (impl_->available == available) return;

    impl_->available = available;
    if (!available) {
        // Links die with the radio; the Java side reports each one separately.
        impl_->advertising = false;
        impl_->discovering = false;
    }
    if (impl_->listener) impl_->listener->onTransportStateChanged(id(), available);
}

// ── JNI entry points — ALL on binder threads ────────────────────────────────
//
// Each one marshals and posts. None of them may reach core state directly.

extern "C" {

JNIEXPORT void JNICALL Java_com_anytimechat_ble_BleRadio_onPeerSeen(
    JNIEnv* env, jclass, jbyteArray peerIdArray, jstring addressString, jint rssi)
{
    if (!g_transport) return;

    const PeerId peerId = peerIdFrom(env, peerIdArray);
    const QString address = QJniObject(addressString).toString();
    const int signal = static_cast<int>(rssi);

    QMetaObject::invokeMethod(
        g_transport, [peerId, address, signal] {
            if (g_transport) g_transport->deliverPeerSeen(peerId, address, signal);
        },
        Qt::QueuedConnection);
}

JNIEXPORT void JNICALL Java_com_anytimechat_ble_BleRadio_onLinkUp(JNIEnv* env, jclass,
                                                                   jint linkId,
                                                                   jbyteArray peerIdArray)
{
    if (!g_transport) return;

    const PeerId peerId = peerIdFrom(env, peerIdArray);
    const LinkId link = static_cast<LinkId>(linkId);

    QMetaObject::invokeMethod(
        g_transport, [link, peerId] {
            if (g_transport) g_transport->deliverLinkUp(link, peerId);
        },
        Qt::QueuedConnection);
}

JNIEXPORT void JNICALL Java_com_anytimechat_ble_BleRadio_onLinkDown(JNIEnv*, jclass, jint linkId,
                                                                     jint reason)
{
    if (!g_transport) return;

    const LinkId link = static_cast<LinkId>(linkId);
    const int why = static_cast<int>(reason);

    QMetaObject::invokeMethod(
        g_transport, [link, why] {
            if (g_transport) g_transport->deliverLinkDown(link, why);
        },
        Qt::QueuedConnection);
}

JNIEXPORT void JNICALL Java_com_anytimechat_ble_BleRadio_onFrame(JNIEnv* env, jclass, jint linkId,
                                                                 jbyteArray frameArray)
{
    if (!g_transport) return;

    // The copy is not optional: the jbyteArray is a local reference that dies
    // when this function returns, long before the queued call runs.
    const QByteArray frame = bytesFrom(env, frameArray);
    const LinkId link = static_cast<LinkId>(linkId);

    QMetaObject::invokeMethod(
        g_transport, [link, frame] {
            if (g_transport) g_transport->deliverFrame(link, frame);
        },
        Qt::QueuedConnection);
}

JNIEXPORT void JNICALL Java_com_anytimechat_ble_BleRadio_onRadioState(JNIEnv*, jclass, jint state)
{
    if (!g_transport) return;

    const bool available = (state != 0);
    QMetaObject::invokeMethod(
        g_transport, [available] {
            if (g_transport) g_transport->deliverRadioState(available);
        },
        Qt::QueuedConnection);
}

}  // extern "C"

bool BleGattTransport::ensurePermissions()
{
    const QJniObject context = QNativeInterface::QAndroidApplication::context();
    if (!context.isValid()) return false;

    const bool granted = QJniObject::callStaticMethod<jboolean>(
        kPermissionClass, "hasBluetoothPermissions", "(Landroid/content/Context;)Z",
        context.object());
    if (granted) return true;

    // Prompt once and report not-yet-granted. AppCore retries start() on a
    // timer; blocking here would freeze the UI thread behind a system dialog.
    if (!g_permissionsRequested) {
        g_permissionsRequested = true;
        AT_LOG_INFO("ble", "asking for Bluetooth permissions");
        QJniObject::callStaticMethod<void>(kPermissionClass, "requestBluetoothPermissions",
                                           "(Landroid/content/Context;)V", context.object());
    }
    return false;
}

bool BleGattTransport::registerNatives()
{
    if (g_nativesRegistered) return true;

    static const JNINativeMethod methods[] = {
        {"onPeerSeen", "([BLjava/lang/String;I)V",
         reinterpret_cast<void*>(Java_com_anytimechat_ble_BleRadio_onPeerSeen)},
        {"onLinkUp", "(I[B)V",
         reinterpret_cast<void*>(Java_com_anytimechat_ble_BleRadio_onLinkUp)},
        {"onLinkDown", "(II)V",
         reinterpret_cast<void*>(Java_com_anytimechat_ble_BleRadio_onLinkDown)},
        {"onFrame", "(I[B)V", reinterpret_cast<void*>(Java_com_anytimechat_ble_BleRadio_onFrame)},
        {"onRadioState", "(I)V",
         reinterpret_cast<void*>(Java_com_anytimechat_ble_BleRadio_onRadioState)},
    };

    QJniEnvironment env;
    g_nativesRegistered =
        env.registerNativeMethods(kRadioClass, methods, sizeof(methods) / sizeof(methods[0]));
    return g_nativesRegistered;
}

}  // namespace anytime::transport
