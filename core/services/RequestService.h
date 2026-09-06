// core/services/RequestService.h
//
// The request → accept gate that makes this app different from bitchat (AD-04).
//
// The accept step is where trust is established: BOTH sides pin the other's
// authenticated Noise static key onto the Contact (§08). That pin is what stops
// a later device reusing a nickname from impersonating a known contact. It is
// captured once and never updated.

#pragma once

#include <string>
#include <vector>

#include "core/domain/Entities.h"
#include "core/domain/Result.h"

namespace anytime {

class IContactRepository;
class IMessageRepository;
class IEventSink;
class IClock;
class MeshEngine;
class SessionManager;
class DiscoveryService;
class AccountService;

class RequestService {
public:
    RequestService(MeshEngine&, SessionManager&, DiscoveryService&, AccountService&,
                   IContactRepository&, IMessageRepository&, IEventSink&, IClock&);

    /// POST /{t}/api/requests — "Say hi" on the Nearby screen. Ensures a Noise
    /// session, then sends chat.request. Fails if the peer is blocked or gone.
    Outcome<Request> send(const PeerId&, const std::string& greeting);

    /// GET /{t}/api/requests
    Outcome<std::vector<Request>> inbound();

    /// POST /{t}/api/requests/:id/accept — pins their static key, creates the
    /// contact and the chat, and replies with chat.accept.
    Outcome<Chat> accept(const RequestId&);

    /// POST /{t}/api/requests/:id/decline — optionally blocks by fingerprint.
    Result decline(const RequestId&, bool alsoBlock);

    // ── driven by MeshEngine ─────────────────────────────────────────────────

    /// Applies the block list, the account's acceptFrom setting and the rate
    /// limit before storing anything. Silently drops rejected requests — telling
    /// a blocked sender they were blocked is itself a signal.
    void onRequestReceived(const PeerId&, const std::string& nickname,
                           const std::string& greeting);

    /// Our outbound request was accepted: pin, create the contact and the chat.
    void onAcceptReceived(const PeerId&, const std::string& nickname);

    void onDeclineReceived(const PeerId&);

private:
    /// Shared by accept() and onAcceptReceived() — both must pin identically.
    Outcome<Chat> establishContact(const PeerId&, const std::string& displayName);

    bool passesAdmissionChecks(const PeerId&);

    MeshEngine&         mesh_;
    SessionManager&     sessions_;
    DiscoveryService&   discovery_;
    AccountService&     accounts_;
    IContactRepository& contacts_;
    IMessageRepository& messages_;
    IEventSink&         events_;
    IClock&             clock_;
};

}  // namespace anytime
