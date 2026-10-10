// core/services/RequestService.cpp
//
// The request gate, and the moment trust is established.
//
// establishContact() is the security-critical function in this file: it pins the
// peer's Noise-authenticated static key onto the Contact. That pin is what stops
// a later device reusing a nickname from impersonating a known contact, and it
// is captured once and never updated (docs/ARCHITECTURE.md section 08).
//
// Logs here carry peer ids and outcomes only — never nicknames or greetings.

#include "core/services/RequestService.h"

#include "core/log/Log.h"
#include "core/ports/IClock.h"
#include "core/ports/IEventSink.h"
#include "core/ports/IRepositories.h"
#include "core/services/AccountService.h"
#include "core/services/DiscoveryService.h"
#include "core/services/MeshEngine.h"
#include "core/services/SessionManager.h"

namespace anytime {

namespace {

/// Ids only have to be unique on this device — they never travel.
std::string newId(const char* prefix, const PeerId& peerId, Timestamp now)
{
    return std::string(prefix) + "_" + peerId.toHex() + "_" + std::to_string(now);
}

}  // namespace

PeerPresentation presentPeer(bool isContact, std::optional<RequestState> outgoing,
                             Timestamp outgoingAgeMillis, Timestamp resendCooldownMillis)
{
    // A contact outranks any request row. Once accepted, the request's state is
    // history and the screen should offer the conversation instead.
    if (isContact) return PeerPresentation{PeerStatus::Contact, false, ChatId{}};

    if (!outgoing.has_value()) return PeerPresentation{PeerStatus::None, true, ChatId{}};

    switch (*outgoing) {
        case RequestState::Pending:
            // Still "Requested" either way — the user is told it was sent. Only
            // whether it may be sent AGAIN depends on the cooldown.
            return PeerPresentation{PeerStatus::Requested,
                                    outgoingAgeMillis >= resendCooldownMillis, ChatId{}};

        case RequestState::Declined:
            // A PLAIN decline reaches us as a chat.decline envelope, so this
            // state is knowable. A decline-WITH-BLOCK deliberately sends nothing
            // (§08: telling someone they were blocked is itself information), so
            // a blocked sender stays on the Pending branch above and their
            // resends are refused by send()'s block check. That silence is the
            // intended behaviour, not a gap.
            return PeerPresentation{PeerStatus::Declined, true, ChatId{}};

        case RequestState::Accepted:
            // Accepted with no contact means establishContact failed — most
            // likely no authenticated session at the time. Let them try again
            // rather than stranding them on a state with no way forward.
            return PeerPresentation{PeerStatus::None, true, ChatId{}};
    }
    return PeerPresentation{PeerStatus::None, true, ChatId{}};
}

RequestService::RequestService(MeshEngine& mesh, SessionManager& sessions,
                               DiscoveryService& discovery, AccountService& accounts,
                               IContactRepository& contacts, IMessageRepository& messages,
                               ITransactionScope& transactions, IEventSink& events, IClock& clock)
    : mesh_(mesh),
      sessions_(sessions),
      discovery_(discovery),
      accounts_(accounts),
      contacts_(contacts),
      messages_(messages),
      transactions_(transactions),
      events_(events),
      clock_(clock)
{
}

// ─────────────────────────────────────────────────────────────────────────────

Outcome<Request> RequestService::send(const PeerId& peerId, const std::string& greeting)
{
    if (contacts_.isBlocked(peerId)) {
        AT_LOG_INFO("request", "request to blocked peer {} refused", peerId.toHex());
        return Outcome<Request>::error(ErrorCode::PermissionDenied, "That person is blocked.");
    }
    if (greeting.size() > kMaxGreetingChars) {
        return Outcome<Request>::error(ErrorCode::InvalidArgument, "Greeting is too long.");
    }

    auto account = accounts_.current();
    if (account.isError()) return account.result();
    if (!account.value().has_value()) {
        return Outcome<Request>::error(ErrorCode::PreconditionFailed, "No account yet.");
    }

    // A session is needed before anything can be encrypted. It may still be
    // handshaking when this returns — the request is stored either way and the
    // envelope is retried once the session lands.
    mesh_.ensureSession(peerId);

    // REUSE the existing outgoing row rather than adding one per tap.
    //
    // Every "Say hi again" used to create a fresh row, because the id embeds a
    // timestamp. The wire was unaffected — the receiver already ignores
    // duplicates — but the rows accumulated locally forever, and the newest one
    // silently became the only one that mattered.
    const std::optional<Request> existing = newestOutgoing(peerId);

    Request request;
    request.id = existing.has_value() ? existing->id
                                      : RequestId{newId("req", peerId, clock_.now())};
    request.peerId = peerId;
    request.direction = Direction::Outgoing;
    request.nickname = account.value()->username;
    request.greeting = greeting;
    // Back to Pending even if the last attempt was declined: this IS the resend,
    // and createdAt restarts the cooldown.
    request.state = RequestState::Pending;
    request.createdAt = clock_.now();

    if (const Result r = contacts_.saveRequest(request); r.isError()) {
        AT_LOG_ERROR("request", "could not store request to {}: {}", peerId.toHex(), r.message());
        return r;
    }

    protocol::AppEnvelope envelope;
    envelope.type = protocol::EnvelopeType::ChatRequest;
    protocol::ChatRequestPayload payload;
    payload.nickname = account.value()->username;
    payload.greeting = greeting;
    envelope.request = payload;

    // A send failure here is not fatal: the request row exists, and the envelope
    // goes out when the session completes.
    const Result sent = mesh_.sendEnvelope(peerId, envelope, PayloadClass::Text);
    AT_LOG_INFO("request", "chat request to {} {}", peerId.toHex(),
                sent.isOk() ? "sent" : "queued until a session is up");

    return request;
}

Outcome<std::vector<Request>> RequestService::inbound()
{
    return contacts_.listRequests(Direction::Incoming, RequestState::Pending);
}

std::optional<Request> RequestService::newestOutgoing(const PeerId& peerId)
{
    // Three queries because listRequests is keyed on (direction, state) and
    // there is no "any state" variant. The peer count on the Nearby screen is a
    // handful, so this is a few dozen local reads — not worth widening the port
    // for until it shows up in a profile.
    std::optional<Request> newest;

    for (const RequestState state :
         {RequestState::Pending, RequestState::Declined, RequestState::Accepted}) {
        auto listed = contacts_.listRequests(Direction::Outgoing, state);
        if (listed.isError()) continue;

        for (const Request& request : listed.value()) {
            if (request.peerId != peerId) continue;
            if (!newest.has_value() || request.createdAt > newest->createdAt) newest = request;
        }
    }
    return newest;
}

PeerPresentation RequestService::presentationFor(const PeerId& peerId)
{
    auto contact = contacts_.findContact(peerId);
    const bool isContact = contact.isOk() && contact.value().has_value()
                           && !contact.value()->isBlocked();

    const std::optional<Request> outgoing = newestOutgoing(peerId);

    std::optional<RequestState> state;
    Timestamp age = 0;
    if (outgoing.has_value()) {
        state = outgoing->state;
        age = clock_.now() - outgoing->createdAt;
    }

    PeerPresentation presentation = presentPeer(isContact, state, age);

    // Only a contact has a chat, and the UI needs its id to open it.
    if (presentation.status == PeerStatus::Contact) {
        auto chat = messages_.findChatByPeer(peerId);
        if (chat.isOk() && chat.value().has_value()) presentation.chatId = chat.value()->id;
    }
    return presentation;
}

Outcome<Chat> RequestService::accept(const RequestId& id)
{
    auto found = contacts_.findRequest(id);
    if (found.isError()) return found.result();
    if (!found.value().has_value()) {
        return Outcome<Chat>::error(ErrorCode::NotFound, "No such request.");
    }

    const Request request = *found.value();
    if (request.state != RequestState::Pending) {
        return Outcome<Chat>::error(ErrorCode::PreconditionFailed,
                                    "That request has already been answered.");
    }

    auto chat = establishContact(request.peerId, request.nickname, id);
    if (chat.isError()) {
        AT_LOG_WARN("request", "could not accept request from {}: {}", request.peerId.toHex(),
                    chat.result().message());
        return chat;
    }

    auto account = accounts_.current();
    protocol::AppEnvelope envelope;
    envelope.type = protocol::EnvelopeType::ChatAccept;
    protocol::ChatAcceptPayload payload;
    payload.nickname =
        account.isOk() && account.value().has_value() ? account.value()->username : std::string();
    envelope.accept = payload;

    mesh_.sendEnvelope(request.peerId, envelope, PayloadClass::Text);

    AT_LOG_INFO("request", "accepted request from {}", request.peerId.toHex());

    Request accepted = request;
    accepted.state = RequestState::Accepted;
    events_.onRequestAccepted(accepted, chat.value());

    return chat;
}

Result RequestService::decline(const RequestId& id, bool alsoBlock)
{
    auto found = contacts_.findRequest(id);
    if (found.isError()) return found.result();
    if (!found.value().has_value()) return Result::error(ErrorCode::NotFound, "No such request.");

    const Request request = *found.value();

    // One unit, so "decline and block" can never end up declined but not
    // blocked — the half the user cared about most.
    const Result stored = transactions_.runInTransaction([&]() -> Result {
        if (const Result r = contacts_.setRequestState(id, RequestState::Declined); r.isError()) {
            return r;
        }
        return alsoBlock ? contacts_.setBlocked(request.peerId, true) : Result::ok();
    });
    if (stored.isError()) {
        AT_LOG_ERROR("request", "could not decline request {}: {}", id.value(), stored.message());
        return stored;
    }
    if (alsoBlock) {
        // Drop the session too: a blocked peer should not keep an open channel.
        sessions_.forget(request.peerId);
    }

    // No chat.decline is sent when blocking. Telling someone they were blocked
    // is itself information, and a blocked sender has no claim to it.
    if (!alsoBlock) {
        protocol::AppEnvelope envelope;
        envelope.type = protocol::EnvelopeType::ChatDecline;
        mesh_.sendEnvelope(request.peerId, envelope, PayloadClass::Text);
    }

    AT_LOG_INFO("request", "declined request from {}{}", request.peerId.toHex(),
                alsoBlock ? " and blocked them" : "");

    events_.onRequestDeclined(id, request.peerId);
    return Result::ok();
}

// ── driven by MeshEngine ─────────────────────────────────────────────────────

void RequestService::onRequestReceived(const PeerId& peerId, const std::string& nickname,
                                       const std::string& greeting)
{
    if (!passesAdmissionChecks(peerId)) return;

    // Already a contact? Then there is nothing to decide — but there may be
    // something to repair. A contact asking again usually means OUR accept never
    // took on their side: it was lost in flight, or their device failed to store
    // it and rolled back. Ignoring them left the pair stuck for good, one side a
    // contact and the other still waiting. Re-sending the accept is idempotent
    // there, and the rate limit above bounds how often a peer can make us do it.
    auto existing = contacts_.findContact(peerId);
    if (existing.isOk() && existing.value().has_value()) {
        AT_LOG_DEBUG("request", "request from existing contact {}; re-sending our accept",
                     peerId.toHex());
        auto account = accounts_.current();
        protocol::AppEnvelope envelope;
        envelope.type = protocol::EnvelopeType::ChatAccept;
        protocol::ChatAcceptPayload payload;
        payload.nickname = account.isOk() && account.value().has_value()
                               ? account.value()->username
                               : std::string();
        envelope.accept = payload;
        mesh_.sendEnvelope(peerId, envelope, PayloadClass::Text);
        return;
    }

    // Already pending from this peer? Retries are normal — the sender resends
    // whenever an ack is lost, and MeshEngine flushes anything queued during a
    // handshake — so a second copy must update nothing rather than stack up a
    // second card in the Requests list.
    auto pending = contacts_.listRequests(Direction::Incoming, RequestState::Pending);
    if (pending.isOk()) {
        for (const Request& existingRequest : pending.value()) {
            if (existingRequest.peerId == peerId) {
                AT_LOG_DEBUG("request", "duplicate request from {} ignored", peerId.toHex());
                return;
            }
        }
    }

    Request request;
    request.id = RequestId{newId("req", peerId, clock_.now())};
    request.peerId = peerId;
    request.direction = Direction::Incoming;
    request.nickname = nickname;
    request.greeting = greeting.substr(0, kMaxGreetingChars);
    request.state = RequestState::Pending;
    request.createdAt = clock_.now();

    if (const Result r = contacts_.saveRequest(request); r.isError()) {
        AT_LOG_ERROR("request", "could not store request from {}: {}", peerId.toHex(),
                     r.message());
        return;
    }

    AT_LOG_INFO("request", "chat request received from {}", peerId.toHex());
    events_.onRequestReceived(request);
}

void RequestService::onAcceptReceived(const PeerId& peerId, const std::string& nickname)
{
    auto pending = contacts_.listRequests(Direction::Outgoing, RequestState::Pending);
    if (pending.isError()) return;

    // Only honour an accept for a request we actually sent. Without this check an
    // unsolicited chat.accept would create a contact and a chat out of nothing,
    // which is precisely the gate this service exists to enforce.
    const Request* match = nullptr;
    for (const Request& request : pending.value()) {
        if (request.peerId == peerId) { match = &request; break; }
    }
    if (match == nullptr) {
        AT_LOG_WARN("request", "unsolicited accept from {} ignored", peerId.toHex());
        return;
    }

    auto chat = establishContact(peerId, nickname.empty() ? match->nickname : nickname, match->id);
    if (chat.isError()) {
        AT_LOG_WARN("request", "accept from {} could not be completed: {}", peerId.toHex(),
                    chat.result().message());
        return;
    }
    AT_LOG_INFO("request", "{} accepted our request", peerId.toHex());

    Request accepted = *match;
    accepted.state = RequestState::Accepted;
    events_.onRequestAccepted(accepted, chat.value());
}

void RequestService::onDeclineReceived(const PeerId& peerId)
{
    auto pending = contacts_.listRequests(Direction::Outgoing, RequestState::Pending);
    if (pending.isError()) return;

    for (const Request& request : pending.value()) {
        if (request.peerId != peerId) continue;
        contacts_.setRequestState(request.id, RequestState::Declined);
        AT_LOG_INFO("request", "{} declined our request", peerId.toHex());
        events_.onRequestDeclined(request.id, peerId);
        return;
    }
}

// ─────────────────────────────────────────────────────────────────────────────

Outcome<Chat> RequestService::establishContact(const PeerId& peerId,
                                               const std::string& displayName,
                                               const RequestId& requestId)
{
    // THE PIN. The key comes from the completed Noise handshake, so it is the
    // key we actually talked to — not a value the peer asserted in a payload,
    // which anyone could forge.
    auto remoteKey = sessions_.remoteStaticKey(peerId);
    if (remoteKey.isError()) {
        return Outcome<Chat>::error(
            ErrorCode::PreconditionFailed,
            "Cannot accept without an authenticated session — try again in range.");
    }

    Contact contact;
    contact.peerId = peerId;
    contact.pinnedStaticKey = remoteKey.value();
    contact.displayName = displayName;
    contact.createdAt = clock_.now();

    std::optional<Chat> chat;
    const Result stored = transactions_.runInTransaction([&]() -> Result {
        if (const Result r = contacts_.addContact(contact); r.isError()) return r;

        // One chat per peer. Re-accepting must not create a second.
        auto existing = messages_.findChatByPeer(peerId);
        if (existing.isError()) return existing.result();
        if (existing.value().has_value()) {
            chat = *existing.value();
        } else {
            auto created = messages_.createChat(peerId);
            if (created.isError()) return created.result();
            chat = created.value();
        }

        return contacts_.setRequestState(requestId, RequestState::Accepted);
    });

    if (stored.isError()) {
        // Rolled back: no contact, no chat, and the request still Pending — so
        // accepting again, or the peer asking again, starts from a clean state.
        AT_LOG_ERROR("request", "could not establish contact {}: {}", peerId.toHex(),
                     stored.message());
        return stored;
    }
    return *chat;
}

bool RequestService::passesAdmissionChecks(const PeerId& peerId)
{
    if (contacts_.isBlocked(peerId)) {
        AT_LOG_DEBUG("request", "request from blocked peer {} dropped", peerId.toHex());
        return false;
    }

    auto account = accounts_.current();
    if (account.isError() || !account.value().has_value()) return false;
    if (account.value()->acceptFrom == AcceptFrom::NoOne) {
        AT_LOG_DEBUG("request", "request from {} dropped: accepting from no one", peerId.toHex());
        return false;
    }

    // Rate limit, enforced locally because no server exists to do it. Advisory
    // against a modified client — the point is to make casual spam unrewarding.
    auto recent = contacts_.countInboundRequestsSince(
        peerId, clock_.now() - kInboundRequestWindowMillis);
    if (recent.isOk() && recent.value() >= kMaxInboundRequestsPerPeer) {
        AT_LOG_WARN("request", "request from {} dropped: rate limit reached", peerId.toHex());
        return false;
    }

    return true;
}

}  // namespace anytime
