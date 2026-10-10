// adapters/transport/nearby/NearbyConnectionsTransport.cpp
//
// JNI binding onto NearbyRadio.java. Read the header first for why this
// transport is secondary and why it only scans in Active mode.
//
// The thread hop is the same discipline as the BLE binding: every native
// callback marshals its arguments and posts to the transport's own thread.
// Nothing here touches core state directly.

#include "adapters/transport/nearby/NearbyConnectionsTransport.h"

#include <QCoreApplication>
#include <QJniEnvironment>
#include <QJniObject>
#include <QMetaObject>
#include <QPointer>

#include "core/domain/Types.h"
#include "core/log/Log.h"

namespace anytime::transport {

namespace {

constexpr const char* kRadioClass      = "com/anytimechat/nearby/NearbyRadio";
constexpr const char* kPermissionClass = "com/anytimechat/PermissionBridge";

/// The single live Nearby transport, so the static JNI callbacks know where to
/// post. One per process, like the BLE radio.
QPointer<NearbyConnectionsTransport> g_transport;

bool g_nativesRegistered = false;

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

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────

struct NearbyConnectionsTransport::Impl {
    NearbyConfig        config;
    ITransportListener* listener = nullptr;
    QJniObject          radio;

    bool started     = false;
    bool available   = false;
    bool advertising = false;
    bool discovering = false;

    /// Nearby speaks opaque endpoint ids; the core speaks PeerId. A link is
    /// reported up before its first ANNOUNCE identifies it, so both maps exist
    /// for the same reason they do in the BLE binding.
    QHash<QString, PeerId> peerByEndpoint;
    QHash<LinkId, PeerId>  peerByLink;
};

// ─────────────────────────────────────────────────────────────────────────────

NearbyConnectionsTransport::NearbyConnectionsTransport(NearbyConfig config, QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>())
{
    impl_->config = std::move(config);
    g_transport = this;
}

NearbyConnectionsTransport::~NearbyConnectionsTransport()
{
    stop();
    if (g_transport == this) g_transport = nullptr;
}

bool NearbyConnectionsTransport::isSupportedOnThisDevice()
{
    const QJniObject context = QNativeInterface::QAndroidApplication::context();
    if (!context.isValid()) return false;

    return QJniObject::callStaticMethod<jboolean>(
        kRadioClass, "isAvailable", "(Landroid/content/Context;)Z", context.object());
}

TransportCapabilities NearbyConnectionsTransport::caps() const
{
    TransportCapabilities caps;
    caps.maxFramePayload = impl_->config.frameSize;

    // High throughput — Wi-Fi Direct once Nearby upgrades the link.
    caps.throughputClass = 2;

    // THE FLAG ROUTINGPOLICY SELECTS ON. Without it a photo would never come
    // here, which is precisely the bug the PayloadClass plumbing fixed.
    caps.supportsBulk = true;

    // NOT a mesh transport: P2P_POINT_TO_POINT reaches one peer at a time and
    // cannot relay. RoutingPolicy uses this to keep handshakes and acks off it.
    caps.supportsMesh = false;

    // Nearby shows a system dialog per connection. RoutingPolicy subtracts
    // heavily for this so a one-line message never triggers a consent prompt.
    caps.requiresUserConsent = true;

    caps.energyCost = EnergyCost::High;
    return caps;
}

bool NearbyConnectionsTransport::isAvailable() const { return impl_->available; }

void NearbyConnectionsTransport::setListener(ITransportListener* listener)
{
    impl_->listener = listener;
}

Result NearbyConnectionsTransport::start(const PeerId& localPeerId)
{
    if (impl_->started && impl_->available) return Result::ok();

    QJniEnvironment env;
    if (!env.isValid()) {
        return Result::error(ErrorCode::TransportUnavailable, "No JNI environment.");
    }

    // Before NearbyRadio is touched: an unregistered native aborts the process
    // on its first callback rather than failing politely.
    if (!registerNatives()) {
        AT_LOG_ERROR("nearby", "could not register the Nearby native callbacks");
        return Result::error(ErrorCode::TransportUnavailable,
                             "Could not register Nearby native methods.");
    }

    const QJniObject context = QNativeInterface::QAndroidApplication::context();
    if (!context.isValid()) {
        return Result::error(ErrorCode::TransportUnavailable, "No Android context.");
    }

    // Defence in depth: main.cpp already refuses to register this transport
    // without Play Services, but start() is retried on a timer and the answer
    // can change if the user installs or enables it.
    if (!isSupportedOnThisDevice()) {
        return Result::error(ErrorCode::TransportUnavailable,
                             "Google Play Services is not available.");
    }

    // NEARBY NEEDS THE BLUETOOTH PERMISSIONS TOO — it drives BLE and classic
    // Bluetooth underneath, whatever its name suggests.
    //
    // Without this gate the radio started into a broken state on first launch:
    // Play Services was present, so start() succeeded and isAvailable() reported
    // true, while every Nearby call failed with MISSING_PERMISSION_BLUETOOTH_
    // ADVERTISE (8038) and _SCAN (8037). AppCore then logged "all transports are
    // up", which was simply false.
    //
    // Deliberately CHECKS WITHOUT PROMPTING: BleGattTransport::ensurePermissions
    // already raises the one dialog, and a second prompt from here would put two
    // system dialogs in front of the user for the same permission set. AppCore
    // retries start() every kTransportRetryMillis, so Nearby comes up by itself
    // moments after the grant.
    const bool granted = QJniObject::callStaticMethod<jboolean>(
        kPermissionClass, "hasBluetoothPermissions", "(Landroid/content/Context;)Z",
        context.object());
    if (!granted) {
        AT_LOG_DEBUG("nearby", "waiting for Bluetooth permission before starting Nearby");
        return Result::error(ErrorCode::PermissionDenied,
                             "Bluetooth permission has not been granted yet.");
    }

    if (!impl_->radio.isValid()) {
        impl_->radio = QJniObject(kRadioClass, "(Landroid/content/Context;)V", context.object());
        if (!impl_->radio.isValid()) {
            AT_LOG_ERROR("nearby", "could not construct NearbyRadio");
            return Result::error(ErrorCode::TransportUnavailable,
                                 "Could not construct NearbyRadio.");
        }
    }

    if (impl_->started) {
        impl_->radio.callMethod<void>("stop");
        impl_->started = false;
    }

    QJniObject serviceId = QJniObject::fromString(impl_->config.serviceId);

    jbyteArray peerBytes = env->NewByteArray(static_cast<jsize>(kPeerIdBytes));
    env->SetByteArrayRegion(peerBytes, 0, static_cast<jsize>(kPeerIdBytes),
                            reinterpret_cast<const jbyte*>(localPeerId.bytes().data()));

    const jboolean ok = impl_->radio.callMethod<jboolean>(
        "start", "(Ljava/lang/String;[B)Z", serviceId.object<jstring>(), peerBytes);
    env->DeleteLocalRef(peerBytes);

    if (!ok) {
        AT_LOG_DEBUG("nearby", "Nearby did not start");
        return Result::error(ErrorCode::TransportUnavailable, "Nearby Connections unavailable.");
    }

    impl_->started = true;
    impl_->available = true;

    AT_LOG_INFO("nearby", "Nearby Connections started");
    return Result::ok();
}

void NearbyConnectionsTransport::stop()
{
    if (!impl_->started) return;
    if (impl_->radio.isValid()) impl_->radio.callMethod<void>("stop");

    impl_->started = false;
    impl_->available = false;
    impl_->advertising = false;
    impl_->discovering = false;
    impl_->peerByEndpoint.clear();
    impl_->peerByLink.clear();

    AT_LOG_INFO("nearby", "Nearby Connections stopped");
}

Result NearbyConnectionsTransport::startDiscovery(DiscoveryMode mode)
{
    if (!impl_->started) {
        return Result::error(ErrorCode::TransportUnavailable, "Nearby not started.");
    }

    // THE BATTERY DECISION. Nearby discovery drives BLE, classic Bluetooth and
    // Wi-Fi scanning simultaneously — far more than the BLE-only duty cycle. So
    // it runs only while someone is actually watching the Nearby screen, which
    // is what DiscoveryMode::Active means. Passive is the background state, and
    // background is exactly when this must not run.
    if (mode != DiscoveryMode::Active) {
        stopDiscovery();
        return Result::ok();
    }

    impl_->radio.callMethod<void>("startScanning");
    impl_->discovering = true;

    AT_LOG_DEBUG("nearby", "discovering (active mode only)");
    return Result::ok();
}

void NearbyConnectionsTransport::stopDiscovery()
{
    if (impl_->radio.isValid()) impl_->radio.callMethod<void>("stopScanning");
    impl_->discovering = false;
}

Result NearbyConnectionsTransport::startAdvertising()
{
    if (!impl_->started) {
        return Result::error(ErrorCode::TransportUnavailable, "Nearby not started.");
    }

    // Advertising stays on whenever the transport is up, unlike discovery. It is
    // the cheaper half, and without it a peer could not open a bulk link to SEND
    // us a photo — only to receive one.
    impl_->radio.callMethod<void>("startAdvertising");
    impl_->advertising = true;
    return Result::ok();
}

void NearbyConnectionsTransport::stopAdvertising()
{
    if (impl_->radio.isValid()) impl_->radio.callMethod<void>("stopAdvertising");
    impl_->advertising = false;
}

Outcome<LinkId> NearbyConnectionsTransport::openLink(const PeerHandle& handle)
{
    if (!impl_->started) {
        return Outcome<LinkId>::error(ErrorCode::TransportUnavailable, "Nearby not started.");
    }

    // PeerHandle::address is transport-scoped and opaque to the core (§06). For
    // BLE it is a MAC; here it is a Nearby endpoint id.
    const QString endpointId = QString::fromStdString(handle.address);
    QJniObject endpointObject = QJniObject::fromString(endpointId);

    const jint linkId = impl_->radio.callMethod<jint>("openLink", "(Ljava/lang/String;)I",
                                                      endpointObject.object<jstring>());
    if (linkId == 0) {
        AT_LOG_DEBUG("nearby", "connection to {} could not be started", handle.peerId.toHex());
        return Outcome<LinkId>::error(ErrorCode::LinkClosed,
                                      "Could not start a Nearby connection.");
    }

    impl_->peerByEndpoint.insert(endpointId, handle.peerId);
    impl_->peerByLink.insert(static_cast<LinkId>(linkId), handle.peerId);

    return static_cast<LinkId>(linkId);
}

void NearbyConnectionsTransport::closeLink(LinkId link)
{
    if (impl_->radio.isValid()) {
        impl_->radio.callMethod<void>("closeLink", "(I)V", static_cast<jint>(link));
    }
    impl_->peerByLink.remove(link);
}

Result NearbyConnectionsTransport::send(LinkId link, ByteSpan frame)
{
    if (!impl_->started) {
        return Result::error(ErrorCode::TransportUnavailable, "Nearby not started.");
    }
    if (frame.size() > impl_->config.frameSize) {
        return Result::error(ErrorCode::InvalidArgument, "Frame exceeds the transport budget.");
    }

    // No broadcast: P2P_POINT_TO_POINT has no such concept, and announces are
    // BLE's job. kInvalidLink here is a caller error, not a fan-out request.
    if (link == kInvalidLink) {
        return Result::error(ErrorCode::InvalidArgument,
                             "Nearby cannot broadcast; announces belong on the mesh transport.");
    }

    QJniEnvironment env;
    jbyteArray array = env->NewByteArray(static_cast<jsize>(frame.size()));
    env->SetByteArrayRegion(array, 0, static_cast<jsize>(frame.size()),
                            reinterpret_cast<const jbyte*>(frame.data()));

    const jboolean ok = impl_->radio.callMethod<jboolean>("send", "(I[B)Z",
                                                          static_cast<jint>(link), array);
    env->DeleteLocalRef(array);

    if (!ok) {
        AT_LOG_DEBUG("nearby", "link {} refused a {}-byte frame", link, frame.size());
        return Result::error(ErrorCode::SendFailed, "Nearby payload refused.");
    }
    return Result::ok();
}

// ── invoked on the CORE thread ───────────────────────────────────────────────

void NearbyConnectionsTransport::deliverPeerSeen(const PeerId& peerId, const QString& endpointId,
                                                 int rssi)
{
    impl_->peerByEndpoint.insert(endpointId, peerId);

    if (!impl_->listener) return;

    PeerHandle handle;
    handle.peerId = peerId;
    handle.address = endpointId.toStdString();
    handle.rssi = rssi;  // always 0: Nearby exposes no signal strength
    impl_->listener->onPeerSeen(id(), handle);
}

void NearbyConnectionsTransport::deliverLinkUp(LinkId link, const PeerId& peerIdFromJava)
{
    PeerId peerId = peerIdFromJava;
    if (peerId.isNull()) peerId = impl_->peerByLink.value(link, PeerId{});

    impl_->peerByLink.insert(link, peerId);
    if (impl_->listener) impl_->listener->onLinkUp(id(), link, peerId);
}

void NearbyConnectionsTransport::deliverLinkDown(LinkId link, int reason)
{
    impl_->peerByLink.remove(link);

    static const LinkCloseReason kReasons[] = {LinkCloseReason::Local, LinkCloseReason::Remote,
                                               LinkCloseReason::Timeout, LinkCloseReason::Error};
    const LinkCloseReason mapped =
        (reason >= 0 && reason < 4) ? kReasons[reason] : LinkCloseReason::Error;

    if (impl_->listener) impl_->listener->onLinkDown(id(), link, mapped);
}

void NearbyConnectionsTransport::deliverFrame(LinkId link, const QByteArray& frame)
{
    if (!impl_->listener) return;
    impl_->listener->onFrame(id(), link,
                             ByteSpan{reinterpret_cast<const Byte*>(frame.constData()),
                                      static_cast<std::size_t>(frame.size())});
}

void NearbyConnectionsTransport::deliverRadioState(bool available)
{
    if (!impl_->started) return;
    if (impl_->available == available) return;

    impl_->available = available;
    if (!available) {
        impl_->advertising = false;
        impl_->discovering = false;
    }
    if (impl_->listener) impl_->listener->onTransportStateChanged(id(), available);
}

// ── JNI entry points — marshal and post, never touch core state ─────────────

extern "C" {

JNIEXPORT void JNICALL Java_com_anytimechat_nearby_NearbyRadio_onPeerSeen(
    JNIEnv* env, jclass, jbyteArray peerIdArray, jstring endpointString, jint rssi)
{
    if (!g_transport) return;

    const PeerId peerId = peerIdFrom(env, peerIdArray);
    const QString endpointId = QJniObject(endpointString).toString();
    const int signal = static_cast<int>(rssi);

    QMetaObject::invokeMethod(
        g_transport, [peerId, endpointId, signal] {
            if (g_transport) g_transport->deliverPeerSeen(peerId, endpointId, signal);
        },
        Qt::QueuedConnection);
}

JNIEXPORT void JNICALL Java_com_anytimechat_nearby_NearbyRadio_onLinkUp(JNIEnv* env, jclass,
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

JNIEXPORT void JNICALL Java_com_anytimechat_nearby_NearbyRadio_onLinkDown(JNIEnv*, jclass,
                                                                          jint linkId,
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

JNIEXPORT void JNICALL Java_com_anytimechat_nearby_NearbyRadio_onFrame(JNIEnv* env, jclass,
                                                                       jint linkId,
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

JNIEXPORT void JNICALL Java_com_anytimechat_nearby_NearbyRadio_onRadioState(JNIEnv*, jclass,
                                                                            jint state)
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

bool NearbyConnectionsTransport::registerNatives()
{
    if (g_nativesRegistered) return true;

    static const JNINativeMethod methods[] = {
        {"onPeerSeen", "([BLjava/lang/String;I)V",
         reinterpret_cast<void*>(Java_com_anytimechat_nearby_NearbyRadio_onPeerSeen)},
        {"onLinkUp", "(I[B)V",
         reinterpret_cast<void*>(Java_com_anytimechat_nearby_NearbyRadio_onLinkUp)},
        {"onLinkDown", "(II)V",
         reinterpret_cast<void*>(Java_com_anytimechat_nearby_NearbyRadio_onLinkDown)},
        {"onFrame", "(I[B)V",
         reinterpret_cast<void*>(Java_com_anytimechat_nearby_NearbyRadio_onFrame)},
        {"onRadioState", "(I)V",
         reinterpret_cast<void*>(Java_com_anytimechat_nearby_NearbyRadio_onRadioState)},
    };

    QJniEnvironment env;
    g_nativesRegistered =
        env.registerNativeMethods(kRadioClass, methods, sizeof(methods) / sizeof(methods[0]));
    return g_nativesRegistered;
}

}  // namespace anytime::transport
