// adapters/transport/ble/BleGattTransport.h
//
// ITransport over the platform BLE radio (AD-02). On Android this is a JNI
// binding onto platform/android/src/com/anytimechat/ble/BleRadio.java.
//
// THE DUMB PIPE RULE (§11). This class and its Java counterpart do exactly six
// things: advertise, scan, open GATT links, close them, push bytes, and report
// four events upward. They contain NO framing, NO TTL, NO fragmentation, NO
// Noise, NO announce scheduling. All of that is core/protocol, written once and
// shared with the iOS port.
//
// If you are tempted to fragment here because MTU lives in the radio: don't.
// Expose the frame size through caps().maxFramePayload and let the core split.
// Fragmentation down here means writing it again in Swift and then debugging two
// reassembly implementations that disagree.
//
// THREADING — THE BUG YOU WILL HIT (§11): Android delivers BLE callbacks on
// binder threads. The core is single-threaded by design. Every JNI callback MUST
// be posted onto the core thread before it touches core state. Skip this and you
// get intermittent corruption under concurrent links that looks like a radio
// problem rather than a threading one. The bounded queue below is not optional.

#pragma once

#include <QHash>
#include <QObject>
#include <QString>

#include <memory>

#include "core/ports/ITransport.h"

namespace anytime::transport {

struct BleConfig {
    /// Must match protocol::kServiceUuid.
    QString serviceUuid;

    /// How big a protocol frame may be. Still never derived from a negotiated
    /// MTU (§11) — that is reported unreliably with several centrals connected.
    /// main.cpp sets this from protocol::kFragmentPayloadBytes; see that constant
    /// for why 480 fits inside one 512-octet ATT write.
    quint32 frameSize = 480;

    /// Android caps advertising at 29 usable bytes, which holds the service UUID
    /// and the 8-byte peer id and nothing else. Nickname and public keys travel
    /// in the ANNOUNCE packet after a link is up.
    quint32 maxConcurrentLinks = 4;
};

class BleGattTransport : public QObject, public ITransport {
    Q_OBJECT

public:
    explicit BleGattTransport(BleConfig, QObject* parent = nullptr);
    ~BleGattTransport() override;

    // ── ITransport ───────────────────────────────────────────────────────────
    TransportId           id()   const override { return TransportId::BleGatt; }
    TransportCapabilities caps() const override;
    bool                  isAvailable() const override;

    Result start(const PeerId& localPeerId) override;
    void   stop() override;

    Result startDiscovery(DiscoveryMode) override;
    void   stopDiscovery() override;
    Result startAdvertising() override;
    void   stopAdvertising() override;

    Outcome<LinkId> openLink(const PeerHandle&) override;
    void            closeLink(LinkId) override;
    Result          send(LinkId, ByteSpan frame) override;

    void setListener(ITransportListener*) override;

    // ── invoked on the CORE thread ───────────────────────────────────────────
    //
    // The JNI callbacks run on binder threads and do nothing but marshal their
    // arguments and post to these via a queued invocation. Calling them directly
    // from a binder thread defeats the entire point — see the .cpp header
    // comment for what that costs.
    void deliverPeerSeen(const PeerId&, const QString& address, int rssi);
    void deliverLinkUp(LinkId, const PeerId&);
    void deliverLinkDown(LinkId, int reason);
    void deliverFrame(LinkId, const QByteArray&);
    void deliverRadioState(bool available);

private:
    /// Binds the five Java native methods to their C++ entry points, once per
    /// process. Must run before BleRadio is constructed.
    static bool registerNatives();

    /// True when every Bluetooth permission is already granted. Otherwise it
    /// prompts (the first time only) and returns false — AppCore retries start()
    /// on a timer rather than blocking on the dialog.
    static bool ensurePermissions();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace anytime::transport
