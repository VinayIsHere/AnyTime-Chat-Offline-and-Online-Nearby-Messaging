// adapters/transport/nearby/NearbyConnectionsTransport.h
//
// ITransport over Google Nearby Connections — the BULK transport (§06).
// JNI binding onto platform/android/src/com/anytimechat/nearby/NearbyRadio.java.
//
// SAME DUMB-PIPE RULE AS BLE (§11). Advertise, discover, open links, push bytes,
// report four events upward. No framing, no TTL, no Noise, no announce
// scheduling. Adding a second transport is supposed to be cheap precisely
// because none of that lives here.
//
// ── WHY THIS IS SECONDARY, NOT PRIMARY ──────────────────────────────────────
//
// BLE carries discovery, presence, handshakes and text. This carries photos.
// The split is forced by the API, not by preference:
//
//   · ONE STRATEGY PER APP. Nearby allows a single Strategy at a time. A general
//     mesh wants P2P_CLUSTER; bulk transfer wants P2P_POINT_TO_POINT, the
//     fastest of the three. Picking CLUSTER to be a general transport would cap
//     photos at the SLOWEST strategy and waste the only reason to be here.
//
//   · PLAY SERVICES. This cannot run without it. Primary would mean a de-Googled
//     phone discovers nobody; secondary means it merely sends photos slowly over
//     BLE. main.cpp checks NearbyRadio.isAvailable() and DOES NOT REGISTER this
//     transport when Play Services is absent — deliberately, because
//     TransportRegistry::allAvailable() requires every registered transport to be
//     usable and AppCore retries on a timer until it is. Registering a
//     permanently-dead transport would spin that retry forever.
//
//   · BATTERY. Nearby discovery drives BLE, classic Bluetooth and Wi-Fi scanning
//     at once. startDiscovery() therefore honours DiscoveryMode: it scans only in
//     Active — someone looking at the Nearby screen — and never in Passive.
//
// ADVERTISING IS ASYMMETRIC WITH DISCOVERY, on purpose: we advertise whenever the
// transport is up so a peer can open a bulk link to us to SEND us a photo, but we
// only discover on demand. Advertising is the cheaper half.
//
// THREADING (§11): Nearby delivers callbacks on the main looper. Every JNI entry
// point marshals and posts to the core thread before touching core state, exactly
// as BleGattTransport does.

#pragma once

#include <QHash>
#include <QObject>
#include <QString>

#include <memory>

#include "core/ports/ITransport.h"

namespace anytime::transport {

struct NearbyConfig {
    /// Nearby's service id namespace. Shares protocol::kServiceUuid with BLE so
    /// the two transports describe the same network.
    QString serviceId;

    /// Far larger than BLE's 480, and that is the entire point: Nearby does its
    /// own reliable segmentation, so the core fragments a photo into tens of
    /// frames instead of a thousand.
    quint32 frameSize = 32 * 1024;
};

class NearbyConnectionsTransport : public QObject, public ITransport {
    Q_OBJECT

public:
    explicit NearbyConnectionsTransport(NearbyConfig, QObject* parent = nullptr);
    ~NearbyConnectionsTransport() override;

    /// True when Google Play Services is present. Call BEFORE constructing and
    /// registering this transport — see the header comment for why registering
    /// an unusable transport is actively harmful.
    static bool isSupportedOnThisDevice();

    // ── ITransport ───────────────────────────────────────────────────────────
    TransportId           id()   const override { return TransportId::NearbyConnections; }
    TransportCapabilities caps() const override;
    bool                  isAvailable() const override;

    Result start(const PeerId& localPeerId) override;
    void   stop() override;

    /// Scans ONLY in DiscoveryMode::Active. Passive and Off both stop the scan.
    Result startDiscovery(DiscoveryMode) override;
    void   stopDiscovery() override;
    Result startAdvertising() override;
    void   stopAdvertising() override;

    Outcome<LinkId> openLink(const PeerHandle&) override;
    void            closeLink(LinkId) override;
    Result          send(LinkId, ByteSpan frame) override;

    void setListener(ITransportListener*) override;

    // ── invoked on the CORE thread, posted from the callback threads ─────────
    void deliverPeerSeen(const PeerId&, const QString& endpointId, int rssi);
    void deliverLinkUp(LinkId, const PeerId&);
    void deliverLinkDown(LinkId, int reason);
    void deliverFrame(LinkId, const QByteArray&);
    void deliverRadioState(bool available);

private:
    /// Binds the five Java native methods, once per process.
    static bool registerNatives();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace anytime::transport
