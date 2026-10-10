// core/domain/Entities.h
//
// The domain model — things with a row in the database and a lifecycle. Plain
// aggregates with no behaviour beyond invariant helpers: business rules live in
// core/services, persistence lives behind the repository ports.
//
// Mirrors the schema in docs/ARCHITECTURE.md section 12. Cross-layer value
// structs that are never stored live in GlobalStructures.h instead; tuning
// constants live in constants/.

#pragma once

#include <optional>
#include <string>

#include "core/domain/Types.h"

namespace anytime {

// ─────────────────────────────────────────────────────────────────────────────
// Account — the single local identity. There is exactly one row (AD-05).
// ─────────────────────────────────────────────────────────────────────────────

struct Account {
    PeerId      peerId;         ///< derived from staticPublicKey, not stored separately
    std::string username;       ///< display label, NOT unique across devices
    std::string dateOfBirth;    ///< ISO-8601 "YYYY-MM-DD"
    Gender      gender = Gender::PreferNotToSay;

    bool       discoverable = true;   ///< false stops advertising entirely
    AcceptFrom acceptFrom   = AcceptFrom::Everyone;

    PublicKey staticPublicKey{};   ///< Curve25519 — identity + Noise
    PublicKey signingPublicKey{};  ///< Ed25519 — signs announces

    Timestamp createdAt = kNoTimestamp;

    /// "@vinay·4f2a" — username plus a fingerprint discriminator.
    std::string displayHandle() const;
};

// ─────────────────────────────────────────────────────────────────────────────
// Peer — someone we have heard announce. Ephemeral; not necessarily a contact.
// ─────────────────────────────────────────────────────────────────────────────

struct Peer {
    PeerId      peerId;
    PublicKey   staticPublicKey{};
    std::string nickname;              ///< as claimed in their ANNOUNCE
    SignalBand  signal = SignalBand::Unknown;

    /// Last time a signature-verified ANNOUNCE arrived. Liveness derives from
    /// this, never from link events — see constants/DiscoveryConstants.h.
    Timestamp verifiedAnnounceAt = kNoTimestamp;
    Timestamp lastSeenAt         = kNoTimestamp;

    /// True while a verified announce sits inside kPeerReachableWindow.
    bool isReachable(Timestamp now) const;

    std::string displayHandle() const;
};

// ─────────────────────────────────────────────────────────────────────────────
// Contact — a peer whose request we accepted, or who accepted ours.
//
// pinnedStaticKey is the trust anchor: captured at accept time and never
// updated. A later device reusing the nickname cannot impersonate this contact.
// ─────────────────────────────────────────────────────────────────────────────

struct Contact {
    PeerId                   peerId;
    PublicKey                pinnedStaticKey{};
    std::string              displayName;
    Timestamp                createdAt = kNoTimestamp;
    std::optional<Timestamp> blockedAt;

    bool isBlocked() const { return blockedAt.has_value(); }
};

// ─────────────────────────────────────────────────────────────────────────────
// Request — a chat request in either direction.
// ─────────────────────────────────────────────────────────────────────────────

struct Request {
    RequestId    id;
    PeerId       peerId;
    Direction    direction = Direction::Incoming;
    std::string  nickname;   ///< snapshot at request time, for the Requests list
    std::string  greeting;   ///< optional, capped at kMaxGreetingChars
    RequestState state     = RequestState::Pending;
    Timestamp    createdAt = kNoTimestamp;
};

// ─────────────────────────────────────────────────────────────────────────────
// Chat — a one-to-one conversation. v1 has no groups; see open question 3.
// ─────────────────────────────────────────────────────────────────────────────

struct Chat {
    ChatId    id;
    PeerId    peerId;
    Timestamp createdAt     = kNoTimestamp;
    Timestamp lastMessageAt = kNoTimestamp;
    int       unreadCount   = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// Message
// ─────────────────────────────────────────────────────────────────────────────

struct Message {
    MessageId       id;
    ChatId          chatId;
    ClientMessageId clientMessageId;  ///< set by the UI for optimistic rendering
    Direction       direction = Direction::Outgoing;
    std::string     body;             ///< plaintext in memory; encrypted at rest
    DeliveryStatus  status    = DeliveryStatus::Queued;

    Timestamp createdAt   = kNoTimestamp;
    Timestamp deliveredAt = kNoTimestamp;
    Timestamp readAt      = kNoTimestamp;

    // ── Media ────────────────────────────────────────────────────────────────
    //
    // An image message carries a REFERENCE here, never the bytes. History paging
    // reads every column of every row it returns, so an inlined image would make
    // opening a chat load megabytes it is not going to display. `body` is the
    // caption for a media message.

    std::string mediaId;        ///< key into the media store; empty for text
    std::string mediaMimeType;  ///< "image/jpeg" or "image/png"
    std::size_t mediaBytes = 0; ///< so the UI can size a placeholder before fetching

    bool isMine()  const { return direction == Direction::Outgoing; }
    bool isMedia() const { return !mediaId.empty(); }
};

// ─────────────────────────────────────────────────────────────────────────────
// OutboxEntry — durable pending send. Survives restart and reboot.
// Retry tuning lives in constants/DeliveryConstants.h.
// ─────────────────────────────────────────────────────────────────────────────

struct OutboxEntry {
    MessageId  messageId;
    PeerId     peerId;
    ByteBuffer payload;        ///< encoded app envelope, ready for a transport
    int        attempts      = 0;
    Timestamp  nextAttemptAt = kNoTimestamp;
    Timestamp  expiresAt     = kNoTimestamp;

    /// Which class of transport this envelope wants (§06).
    ///
    /// PERSISTED, and that is the point: the decision belongs to whoever composed
    /// the message — a photo is Bulk, a line of text is Text — and it has to
    /// survive the app being killed. Re-deriving it at send time from the packet
    /// type cannot work, because every app payload is a NoiseMessage by then and
    /// the distinction has already been lost.
    PayloadClass payloadClass = PayloadClass::Text;
};

}  // namespace anytime
