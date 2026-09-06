// core/domain/Enums.cpp
//
// Every enum conversion in one place, matching Enums.h.
//
// The string forms of DeliveryStatus, RequestState, SignalBand and EventType are
// part of the local API contract the React client reads — changing one is a
// breaking UI change, not a cosmetic edit.

#include "core/domain/Enums.h"

namespace anytime {

const char* toString(ErrorCode code) noexcept
{
    switch (code) {
        case ErrorCode::Ok:                   return "ok";

        case ErrorCode::InvalidArgument:      return "invalid_argument";
        case ErrorCode::NotFound:             return "not_found";
        case ErrorCode::AlreadyExists:        return "already_exists";
        case ErrorCode::PreconditionFailed:   return "precondition_failed";
        case ErrorCode::PermissionDenied:     return "permission_denied";

        case ErrorCode::TransportUnavailable: return "transport_unavailable";
        case ErrorCode::LinkClosed:           return "link_closed";
        case ErrorCode::SendFailed:           return "send_failed";
        case ErrorCode::Timeout:              return "timeout";

        case ErrorCode::MalformedPacket:      return "malformed_packet";
        case ErrorCode::UnsupportedVersion:   return "unsupported_version";
        case ErrorCode::HandshakeFailed:      return "handshake_failed";
        case ErrorCode::DecryptFailed:        return "decrypt_failed";
        case ErrorCode::SignatureInvalid:     return "signature_invalid";

        case ErrorCode::StorageFailure:       return "storage_failure";
        case ErrorCode::Internal:             return "internal";
    }
    return "internal";
}

const char* toString(Gender g) noexcept
{
    switch (g) {
        case Gender::Male:           return "Male";
        case Gender::Female:         return "Female";
        case Gender::Other:          return "Other";
        case Gender::PreferNotToSay: return "Prefer not to say";
    }
    return "Prefer not to say";
}

const char* toString(DeliveryStatus s) noexcept
{
    switch (s) {
        case DeliveryStatus::Queued:    return "queued";
        case DeliveryStatus::Sent:      return "sent";
        case DeliveryStatus::Delivered: return "delivered";
        case DeliveryStatus::Read:      return "read";
        case DeliveryStatus::Failed:    return "failed";
    }
    return "queued";
}

const char* toString(RequestState s) noexcept
{
    switch (s) {
        case RequestState::Pending:  return "pending";
        case RequestState::Accepted: return "accepted";
        case RequestState::Declined: return "declined";
    }
    return "pending";
}

const char* toString(SignalBand b) noexcept
{
    switch (b) {
        case SignalBand::VeryStrong: return "very strong";
        case SignalBand::Strong:     return "strong";
        case SignalBand::Weak:       return "weak";
        case SignalBand::Unknown:    return "unknown";
    }
    return "unknown";
}

const char* toString(TransportId t) noexcept
{
    switch (t) {
        case TransportId::Loopback:          return "loopback";
        case TransportId::BleGatt:           return "ble";
        case TransportId::NearbyConnections: return "nearby";
        case TransportId::Multipeer:         return "multipeer";
        case TransportId::Internet:          return "internet";
    }
    return "unknown";
}

const char* toString(EventType e) noexcept
{
    switch (e) {
        case EventType::PeerDiscovered:  return "peer.discovered";
        case EventType::PeerLost:        return "peer.lost";
        case EventType::RequestReceived: return "request.received";
        case EventType::RequestAccepted: return "request.accepted";
        case EventType::RequestDeclined: return "request.declined";
        case EventType::MessageReceived: return "message.received";
        case EventType::MessageStatus:   return "message.status";
        case EventType::RadioState:      return "radio.state";
    }
    return "unknown";
}

std::optional<Gender> genderFromString(const std::string& s) noexcept
{
    if (s == "Male")   return Gender::Male;
    if (s == "Female") return Gender::Female;
    if (s == "Other")  return Gender::Other;
    if (s == "Prefer not to say") return Gender::PreferNotToSay;
    return std::nullopt;
}

std::optional<AcceptFrom> acceptFromString(const std::string& s) noexcept
{
    if (s == "everyone") return AcceptFrom::Everyone;
    if (s == "nobody" || s == "no-one" || s == "none") return AcceptFrom::NoOne;
    return std::nullopt;
}

}  // namespace anytime
