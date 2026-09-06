// core/domain/Enums.h
//
// Every enumeration in the core, in one place.
//
// Grouped by the namespace they belong to rather than by the file that used to
// hold them. Values that appear on the wire or in the database are numbered
// explicitly and must never be renumbered — add new values at the end.

#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace anytime {

// ─────────────────────────────────────────────────────────────────────────────
// Errors
// ─────────────────────────────────────────────────────────────────────────────

enum class ErrorCode {
    Ok = 0,

    // Caller errors
    InvalidArgument,
    NotFound,
    AlreadyExists,
    PreconditionFailed,   ///< e.g. sending before an account exists
    PermissionDenied,     ///< blocked peer, or acceptFrom = NoOne

    // Transport / link
    TransportUnavailable, ///< radio off, adapter missing, not started
    LinkClosed,
    SendFailed,
    Timeout,

    // Protocol
    MalformedPacket,
    UnsupportedVersion,
    HandshakeFailed,
    DecryptFailed,
    SignatureInvalid,

    // Storage
    StorageFailure,

    Internal
};

// ─────────────────────────────────────────────────────────────────────────────
// Account and profile
// ─────────────────────────────────────────────────────────────────────────────

enum class Gender : std::uint8_t { Male = 0, Female = 1, Other = 2, PreferNotToSay = 3 };

/// Who may send this account a chat request.
enum class AcceptFrom : std::uint8_t { Everyone = 0, NoOne = 1 };

// ─────────────────────────────────────────────────────────────────────────────
// Messaging
// ─────────────────────────────────────────────────────────────────────────────

enum class Direction : std::uint8_t { Incoming = 0, Outgoing = 1 };

/// Delivery lifecycle — see docs/ARCHITECTURE.md section 09. Persisted.
enum class DeliveryStatus : std::uint8_t {
    Queued = 0, Sent = 1, Delivered = 2, Read = 3, Failed = 4
};

enum class RequestState : std::uint8_t { Pending = 0, Accepted = 1, Declined = 2 };

// ─────────────────────────────────────────────────────────────────────────────
// Discovery and transport
// ─────────────────────────────────────────────────────────────────────────────

/// Coarse signal strength. Deliberately banded rather than raw dBm: RSSI is
/// noisy, and the UI only ever shows "very strong / strong / weak".
enum class SignalBand : std::uint8_t { VeryStrong = 0, Strong = 1, Weak = 2, Unknown = 3 };

/// Stable identifier for a registered transport. Used by RoutingPolicy.
enum class TransportId : std::uint8_t {
    Loopback = 0, BleGatt = 1, NearbyConnections = 2, Multipeer = 3, Internet = 4
};

enum class EnergyCost : std::uint8_t { Low = 0, Medium = 1, High = 2 };

/// How aggressively to look for peers. Maps onto duty cycling in phase 4.
enum class DiscoveryMode : std::uint8_t {
    Off = 0,
    Passive = 1,  ///< low duty cycle, background
    Active = 2    ///< user is on the Nearby screen — scan hard
};

/// Why a link went down. Informational — liveness comes from announces.
enum class LinkCloseReason : std::uint8_t { Local = 0, Remote = 1, Timeout = 2, Error = 3 };

/// What a payload is for. RoutingPolicy maps this onto a transport.
enum class PayloadClass : std::uint8_t {
    Control = 0,  ///< handshakes, announces, acks — must use the mesh transport
    Text    = 1,  ///< chat messages — low bandwidth is fine
    Bulk    = 2   ///< images and video — prefers high throughput (phase 5)
};

// ─────────────────────────────────────────────────────────────────────────────
// Crypto
// ─────────────────────────────────────────────────────────────────────────────

enum class HandshakeRole : std::uint8_t { Initiator = 0, Responder = 1 };

enum class SessionState : std::uint8_t {
    None = 0, Handshaking = 1, Established = 2, Failed = 3
};

// ─────────────────────────────────────────────────────────────────────────────
// Events pushed to the UI over SSE. Wire names must stay stable — they are part
// of the local API contract the React client subscribes to.
// ─────────────────────────────────────────────────────────────────────────────

enum class EventType : std::uint8_t {
    PeerDiscovered,   ///< "peer.discovered"
    PeerLost,         ///< "peer.lost"
    RequestReceived,  ///< "request.received"
    RequestAccepted,  ///< "request.accepted"
    RequestDeclined,  ///< "request.declined"
    MessageReceived,  ///< "message.received"
    MessageStatus,    ///< "message.status"
    RadioState        ///< "radio.state"
};

// ── conversions ──────────────────────────────────────────────────────────────

const char* toString(ErrorCode) noexcept;
const char* toString(Gender) noexcept;
const char* toString(DeliveryStatus) noexcept;
const char* toString(RequestState) noexcept;
const char* toString(SignalBand) noexcept;
const char* toString(TransportId) noexcept;
const char* toString(EventType) noexcept;

std::optional<Gender>     genderFromString(const std::string&) noexcept;
std::optional<AcceptFrom> acceptFromString(const std::string&) noexcept;

}  // namespace anytime

// ─────────────────────────────────────────────────────────────────────────────

namespace anytime::protocol {

/// On the wire. Never renumber.
enum class PacketType : std::uint8_t {
    Announce       = 0x01,  ///< signed, cleartext — nickname + public keys
    NoiseHandshake = 0x02,  ///< XX pattern, three messages
    NoiseMessage   = 0x03,  ///< encrypted envelope, carries every app payload
    Fragment       = 0x04,  ///< 8-byte fragment id, index, total
    Ack            = 0x05   ///< delivery confirmation for one message id
};

/// Application payload tags, inside NOISE_MESSAGE. Never renumber.
enum class EnvelopeType : std::uint8_t {
    ChatRequest = 1,
    ChatAccept  = 2,
    ChatDecline = 3,
    ChatMessage = 4,
    ChatRead    = 5
};

}  // namespace anytime::protocol
