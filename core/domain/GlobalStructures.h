// core/domain/GlobalStructures.h
//
// Value structs that cross layer boundaries — shared vocabulary rather than
// domain entities.
//
// The distinction from Entities.h: an entity is something with a row in the
// database and a lifecycle (Account, Peer, Contact, Chat, Message). What lives
// here is passed between layers but never stored — a transport handle, a
// capability advertisement, a page of results.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/domain/Enums.h"
#include "core/domain/Identifiers.h"

namespace anytime {

// ─────────────────────────────────────────────────────────────────────────────
// Transport vocabulary
// ─────────────────────────────────────────────────────────────────────────────

/// What RoutingPolicy reads to choose a transport for a given PayloadClass.
struct TransportCapabilities {
    /// Usable payload bytes per frame, after the transport's own headers. The
    /// core fragments to this — it never queries MTU itself.
    std::uint32_t maxFramePayload = 0;

    /// 0 = Low (BLE), 1 = Medium, 2 = High (Wi-Fi class).
    std::uint32_t throughputClass = 0;

    bool supportsBulk        = false;  ///< worth sending images/video over
    bool supportsMesh        = false;  ///< can relay for third parties
    bool requiresUserConsent = false;  ///< Nearby and Multipeer show system UI

    EnergyCost energyCost = EnergyCost::Low;
};

/// A peer as a transport sees it, before the core knows anything about them.
///
/// `address` is transport-scoped and opaque to the core (a BLE MAC, a socket
/// endpoint, an endpoint id). Never persist it and never compare it across
/// transports — the same person reached two ways has two addresses but one
/// PeerId, which is exactly what makes the phase 5 handoff possible.
struct PeerHandle {
    PeerId      peerId;
    std::string address;
    int         rssi = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// Identity
// ─────────────────────────────────────────────────────────────────────────────

/// The device's long-term identity. Secret halves never appear here — they stay
/// wrapped inside the identity store.
struct Identity {
    PeerId    peerId;              ///< first 8 bytes of SHA-256(staticPublicKey)
    PublicKey staticPublicKey{};   ///< Curve25519 — Noise + identity
    PublicKey signingPublicKey{};  ///< Ed25519 — signs ANNOUNCE packets
};

// ─────────────────────────────────────────────────────────────────────────────
// Query results
// ─────────────────────────────────────────────────────────────────────────────

struct Message;  // defined in Entities.h

/// One page of chat history, newest first.
struct MessagePage {
    std::vector<Message> messages;
    Cursor               nextCursor;
    bool                 hasMore = false;
};

}  // namespace anytime
