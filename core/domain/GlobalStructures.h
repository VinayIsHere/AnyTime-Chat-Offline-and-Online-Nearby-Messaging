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

/// A Chat joined with everything the chat list renders — the read model behind
/// GET /chats and GET /chats/:id.
///
/// A bare Chat carries only a PeerId, so serialising one on its own produces a
/// row with no name: the display name lives on the Contact and the preview lives
/// on the last Message. Joining them here, once, is what stops each caller
/// rediscovering that and passing an empty handle.
///
/// `displayName` is the PINNED contact name, never the peer's live announced
/// nickname — trust comes from the key pinned at accept time, so a peer renaming
/// themselves must not change how an existing chat reads (section 08).
struct ChatSummary {
    ChatId      id;
    PeerId      peerId;
    std::string displayName;
    std::string lastMessage;
    Timestamp   lastMessageAt = kNoTimestamp;
    int         unreadCount   = 0;

    /// Live radio state, filled in by the caller that can see DiscoveryService.
    /// Storage knows nothing about who is currently in range.
    bool reachable = false;

    /// We blocked this contact. Their messages are dropped on arrival and ours
    /// are refused, so the chat screen swaps its composer for an unblock prompt
    /// rather than offering a send button that can only fail.
    bool blocked = false;
};

/// One row of the block list — the read model behind GET /blocked.
///
/// A block is keyed on the key fingerprint alone, and most blocked peers are
/// NOT contacts: declining a request with "block" is the common path. So the
/// name is the best one available, in trust order — the pinned contact name,
/// else the nickname from their last request, else their last announce. It may
/// be empty for a peer we only ever saw as an id; the UI falls back to the
/// fingerprint handle then, which is the part that actually identifies them.
struct BlockedPeer {
    PeerId      peerId;
    std::string displayName;
    Timestamp   blockedAt = kNoTimestamp;
};

}  // namespace anytime
