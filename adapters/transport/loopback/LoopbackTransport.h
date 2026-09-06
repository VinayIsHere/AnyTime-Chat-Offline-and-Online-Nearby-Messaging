// adapters/transport/loopback/LoopbackTransport.h
//
// AD-07 — A DELIVERABLE, NOT A TEST DOUBLE.
//
// We deliberately never write a desktop radio: BLE lives in Java (Android) and
// Swift (iOS) only. So the development machine has ZERO radio, by design, and
// this is the only way anything runs there.
//
// What it buys:
//   · the whole product exists before any radio does — phase 0 delivers
//     discovery → request → accept → chat with no Bluetooth code written
//   · seconds per iteration instead of minutes; breakpoints on both sides of a
//     conversation in one debugger
//   · deliberate hostility — the impairment knobs below are how you actually
//     test outbox retry, dedup and reassembly timeouts. You cannot reproduce
//     30% packet loss reliably by walking around with handsets.
//   · CI: a test binary spins up N instances in-process and runs whole
//     multi-device scenarios headlessly
//   · it proves the seam is real while the seam is still soft. Two independent
//     implementations is the minimum needed to know ITransport is not
//     accidentally shaped around BLE.
//
// BECAUSE IT IS A DELIVERABLE it lives here, not in tests/; it is held to
// production quality; and the desktop build is a supported configuration run
// every day, so it cannot silently rot.

#pragma once

#include <QObject>

#include <memory>

#include "core/ports/ITransport.h"

namespace anytime::transport {

/// Deliberate degradation, for testing the reliability layer. All default to
/// "perfect link" so ordinary development is not noisy.
struct Impairment {
    double lossRate      = 0.0;  ///< 0.0–1.0, applied per frame
    int    latencyMillis = 0;    ///< added delay before delivery
    int    jitterMillis  = 0;    ///< +/- randomisation on latency
    bool   reorder       = false;///< occasionally deliver out of order
    double linkDropRate  = 0.0;  ///< chance a link dies mid-conversation
};

struct LoopbackConfig {
    /// Instances rendezvous by writing {peerId, port} here. Defaults to a
    /// per-user temp directory so several checkouts do not collide.
    QString rendezvousDir;

    /// Discovery scan interval over the rendezvous directory.
    int scanIntervalMillis = 1000;

    /// Simulated RSSI reported to the core, so SignalBand logic is exercised.
    int simulatedRssi = -55;

    Impairment impairment;
};

/// Peer discovery over a rendezvous directory; links over local TCP sockets.
///
/// PLANNED UPGRADE (§AD-07): when relay work begins, replace direct connections
/// with a small hub process that models topology — A hears B, B hears C, A
/// cannot hear C — giving deterministic multi-hop testing on one machine.
class LoopbackTransport : public QObject, public ITransport {
    Q_OBJECT

public:
    explicit LoopbackTransport(LoopbackConfig, QObject* parent = nullptr);
    ~LoopbackTransport() override;

    // ── ITransport ───────────────────────────────────────────────────────────
    TransportId           id()   const override { return TransportId::Loopback; }
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

    /// Adjustable at runtime so a test can turn hostility on mid-scenario.
    void setImpairment(const Impairment&);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace anytime::transport
