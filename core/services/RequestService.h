// core/services/RequestService.h
//
// The request → accept gate that makes this app different from bitchat (AD-04).
//
// The accept step is where trust is established: BOTH sides pin the other's
// authenticated Noise static key onto the Contact (§08). That pin is what stops
// a later device reusing a nickname from impersonating a known contact. It is
// captured once and never updated.

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/domain/Entities.h"
#include "core/domain/Result.h"

namespace anytime {

/// How the Nearby screen should show one peer.
///
/// `status` and `canResend` are SEPARATE on purpose. A request that has gone
/// unanswered past the cooldown is still `Requested` — the screen must keep
/// saying it was sent while letting the user try again. Collapsing the two would
/// either lose that information or lock the button forever.
struct PeerPresentation {
    PeerStatus status    = PeerStatus::None;
    bool       canResend = true;

    /// Set only when `status` is Contact, so the UI can open the chat without
    /// reconstructing its id from the peer id.
    ChatId chatId;
};

/// The presentation decision, as a pure function of the facts.
///
/// Extracted from the service so the cooldown boundary is testable without
/// standing up a MeshEngine, a SessionManager and three repositories — that
/// boundary is exactly the kind of off-by-one nobody notices until a button
/// stays grey.
PeerPresentation presentPeer(bool isContact, std::optional<RequestState> outgoing,
                             Timestamp outgoingAgeMillis,
                             Timestamp resendCooldownMillis = kRequestResendCooldownMillis);

class IContactRepository;
class IMessageRepository;
class ITransactionScope;
class IEventSink;
class IClock;
class MeshEngine;
class SessionManager;
class DiscoveryService;
class AccountService;

class RequestService {
public:
    RequestService(MeshEngine&, SessionManager&, DiscoveryService&, AccountService&,
                   IContactRepository&, IMessageRepository&, ITransactionScope&, IEventSink&,
                   IClock&);

    /// POST /{t}/api/requests — "Say hi" on the Nearby screen. Ensures a Noise
    /// session, then sends chat.request. Fails if the peer is blocked or gone.
    Outcome<Request> send(const PeerId&, const std::string& greeting);

    /// GET /{t}/api/requests
    Outcome<std::vector<Request>> inbound();

    /// How the Nearby screen should present this peer. Reads the contact list
    /// and the newest outgoing request; stores nothing.
    PeerPresentation presentationFor(const PeerId&);

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
    /// Writes the contact, the chat and the request's Accepted state as ONE
    /// transaction: a contact without a chat is a state the request gate can
    /// never recover from, because the peer's later requests are then ignored
    /// as coming from someone already known.
    Outcome<Chat> establishContact(const PeerId&, const std::string& displayName,
                                   const RequestId& request);

    /// The most recent outgoing request to this peer, in any state. Used both to
    /// present the peer and to reuse the row when sending again.
    std::optional<Request> newestOutgoing(const PeerId&);

    bool passesAdmissionChecks(const PeerId&);

    MeshEngine&         mesh_;
    SessionManager&     sessions_;
    DiscoveryService&   discovery_;
    AccountService&     accounts_;
    IContactRepository& contacts_;
    IMessageRepository& messages_;
    ITransactionScope&  transactions_;
    IEventSink&         events_;
    IClock&             clock_;
};

}  // namespace anytime
