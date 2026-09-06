// core/ports/ITransport.h
//
// One radio or link technology. THE key seam of the whole architecture
// (docs/ARCHITECTURE.md sections 06 and 11).
//
// v1 ships two implementations:
//   adapters/transport/ble/       — BleGattTransport, JNI onto BleRadio.java
//   adapters/transport/loopback/  — LoopbackTransport, local sockets  (AD-07)
//
// Phase 5 adds NearbyConnections, Multipeer and Internet transports as new
// classes here. Nothing above this interface changes when they arrive.
//
// THE BINDING RULE (AD-02): implementations are a DUMB PIPE. Bytes in, bytes
// out. No framing, no TTL, no fragmentation, no Noise, no announce scheduling —
// all of that is core/protocol, written once and shared by every platform.
// If you are tempted to put fragmentation here because MTU lives in the radio:
// don't. Expose the frame size through caps() and let the core split.
//
// TransportId, DiscoveryMode, LinkCloseReason and EnergyCost live in
// core/domain/Enums.h; TransportCapabilities and PeerHandle in GlobalStructures.h.

#pragma once

#include "core/domain/Result.h"
#include "core/domain/Types.h"

namespace anytime {

/// Callbacks from a transport into the core.
///
/// THREADING (section 11): implementations MUST deliver these on the core
/// thread. The Android BLE stack calls back on binder threads, so
/// BleGattTransport hops via a bounded queue before invoking any of this. Do not
/// relax that — the failure mode is intermittent corruption under concurrent
/// links that looks like a radio problem rather than a threading one.
class ITransportListener {
public:
    virtual ~ITransportListener() = default;

    /// An advertising peer was observed. Fires repeatedly as adverts arrive; the
    /// core deduplicates and applies the reachability window.
    virtual void onPeerSeen(TransportId, const PeerHandle&) = 0;

    virtual void onLinkUp(TransportId, LinkId, const PeerId&)     = 0;
    virtual void onLinkDown(TransportId, LinkId, LinkCloseReason) = 0;

    /// One complete transport frame. May be a protocol fragment — reassembly is
    /// the core's job, not the transport's.
    virtual void onFrame(TransportId, LinkId, ByteSpan frame) = 0;

    /// Radio availability changed (adapter toggled, permission revoked).
    virtual void onTransportStateChanged(TransportId, bool available) = 0;
};

// ─────────────────────────────────────────────────────────────────────────────

class ITransport {
public:
    virtual ~ITransport() = default;

    virtual TransportId           id()   const = 0;
    virtual TransportCapabilities caps() const = 0;

    /// True once start() has succeeded and the underlying radio is usable.
    virtual bool isAvailable() const = 0;

    /// `localPeerId` is advertised so others can identify us before connecting.
    /// The 29-byte advertising budget holds the service UUID and this id only —
    /// nickname and public keys travel in the ANNOUNCE packet afterwards.
    virtual Result start(const PeerId& localPeerId) = 0;
    virtual void   stop()                           = 0;

    virtual Result startDiscovery(DiscoveryMode) = 0;
    virtual void   stopDiscovery()               = 0;

    /// Advertising is separate from discovery: a non-discoverable account still
    /// scans and connects, it just does not announce itself.
    virtual Result startAdvertising() = 0;
    virtual void   stopAdvertising()  = 0;

    virtual Outcome<LinkId> openLink(const PeerHandle&) = 0;
    virtual void            closeLink(LinkId)           = 0;

    /// Frame must already fit caps().maxFramePayload — the core guarantees this.
    virtual Result send(LinkId, ByteSpan frame) = 0;

    /// Observer, not Qt signals: core stays Qt-free. Single listener; the core
    /// registers exactly one (TransportRegistry).
    virtual void setListener(ITransportListener*) = 0;
};

}  // namespace anytime
