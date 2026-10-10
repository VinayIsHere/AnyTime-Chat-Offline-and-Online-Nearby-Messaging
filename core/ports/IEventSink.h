// core/ports/IEventSink.h
//
// The core's outbound notification channel. The HTTP adapter implements this and
// turns each event into an SSE frame on GET /{token}/api/events (§05).
//
// This is what replaces the 2.5 s polling loop in the NearbyScreen draft. The UI
// never polls; it subscribes once and receives everything below.

#pragma once

#include <string>

#include "core/domain/Entities.h"
#include "core/domain/Types.h"

namespace anytime {

class IEventSink {
public:
    virtual ~IEventSink() = default;

    virtual void onPeerDiscovered(const Peer&) = 0;
    virtual void onPeerLost(const PeerId&)     = 0;

    virtual void onRequestReceived(const Request&) = 0;
    virtual void onRequestAccepted(const Request&, const Chat&) = 0;
    /// The PeerId travels with the id because the Nearby screen keys everything
    /// on peers, not on request ids — without it the screen cannot tell WHICH
    /// peer to offer again, and would have to refetch the whole list to find out.
    virtual void onRequestDeclined(const RequestId&, const PeerId&) = 0;

    virtual void onMessageReceived(const Message&) = 0;
    virtual void onMessageStatusChanged(const MessageId&, DeliveryStatus) = 0;

    /// `detail` carries a short reason for the UI ("bluetooth_off",
    /// "permission_denied") so the Nearby screen can explain itself.
    virtual void onRadioStateChanged(bool available, const std::string& detail) = 0;
};

}  // namespace anytime
