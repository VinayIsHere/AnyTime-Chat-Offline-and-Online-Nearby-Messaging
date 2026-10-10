// core/services/SessionManager.cpp
//
// Session bookkeeping. The keying discipline is the important part: sessions
// are keyed on PeerId, never LinkId — see the header for why that matters to the
// phase 5 transport handoff.

#include "core/services/SessionManager.h"

#include "core/log/Log.h"
#include "core/ports/IClock.h"

namespace anytime {

SessionManager::SessionManager(ISecureSessionFactory& factory, IClock& clock)
    : factory_(factory), clock_(clock)
{
}

SessionManager::~SessionManager() = default;

void SessionManager::setLocalPeerId(const PeerId& peerId)
{
    localPeerId_ = peerId;
}

SessionState SessionManager::stateFor(const PeerId& peerId) const
{
    const auto it = sessions_.find(peerId);
    return it == sessions_.end() ? SessionState::None : it->second.state;
}

Result SessionManager::beginHandshake(const PeerId& peerId, ByteBuffer& out)
{
    out.clear();

    Entry& entry = sessions_[peerId];
    if (entry.state == SessionState::Established) return Result::ok();
    if (entry.state == SessionState::Handshaking) return Result::ok();

    auto created = factory_.createSession(nullptr);
    if (created.isError()) {
        AT_LOG_ERROR("session", "could not create a session for {}: {}", peerId.toHex(),
                     created.result().message());
        sessions_.erase(peerId);
        return created.result();
    }

    entry.session = std::move(created).value();
    entry.role = HandshakeRole::Initiator;
    entry.startedAt = clock_.now();

    const Result r = entry.session->beginHandshake(HandshakeRole::Initiator, out);
    entry.state = r.isOk() ? SessionState::Handshaking : SessionState::Failed;

    if (r.isError()) {
        AT_LOG_WARN("session", "could not begin handshake with {}: {}", peerId.toHex(),
                    r.message());
    } else {
        AT_LOG_DEBUG("session", "handshake with {} started as initiator", peerId.toHex());
    }
    return r;
}

Result SessionManager::readHandshake(const PeerId& peerId, ByteSpan message, ByteBuffer& out)
{
    out.clear();

    const auto existing = sessions_.find(peerId);
    if (existing != sessions_.end() && existing->second.session
        && factory_.isHandshakeInitiation(message)) {
        const Entry& current = existing->second;
        const bool crossed = current.state == SessionState::Handshaking
                             && current.role == HandshakeRole::Initiator;

        // Crossed handshake — see the header. Our own message 1 is already on its
        // way to them, and they will answer it once they yield.
        if (crossed && !localPeerId_.isNull() && localPeerId_ < peerId) {
            AT_LOG_DEBUG("session", "crossed handshake with {}: keeping the initiator role",
                         peerId.toHex());
            return Result::ok();
        }

        if (crossed) {
            AT_LOG_DEBUG("session", "crossed handshake with {}: yielding, answering as responder",
                         peerId.toHex());
        } else {
            AT_LOG_INFO("session", "{} started a new handshake; replacing the old session",
                        peerId.toHex());
        }
        sessions_.erase(existing);
    }

    Entry& entry = sessions_[peerId];

    if (!entry.session) {
        auto created = factory_.createSession(nullptr);
        if (created.isError()) {
            AT_LOG_ERROR("session", "could not create a session for {}: {}", peerId.toHex(),
                         created.result().message());
            sessions_.erase(peerId);
            return created.result();
        }
        entry.session = std::move(created).value();
        entry.role = HandshakeRole::Responder;
        entry.startedAt = clock_.now();
        entry.state = SessionState::Handshaking;

        ByteBuffer ignored;
        if (const Result r = entry.session->beginHandshake(HandshakeRole::Responder, ignored);
            r.isError()) {
            entry.state = SessionState::Failed;
            AT_LOG_WARN("session", "could not start responder handshake with {}: {}",
                        peerId.toHex(), r.message());
            return r;
        }
        AT_LOG_DEBUG("session", "answering a handshake from {}", peerId.toHex());
    }

    const Result r = entry.session->readHandshake(message, out);
    if (r.isError()) {
        entry.state = SessionState::Failed;
        AT_LOG_WARN("session", "handshake with {} failed: {}", peerId.toHex(), r.message());
        return r;
    }

    if (entry.session->isEstablished() && entry.state != SessionState::Established) {
        entry.state = SessionState::Established;
        AT_LOG_INFO("session", "secure session with {} established (as {})", peerId.toHex(),
                    entry.role == HandshakeRole::Initiator ? "initiator" : "responder");
    }
    return Result::ok();
}

bool SessionManager::expireIfStale(const PeerId& peerId)
{
    const auto it = sessions_.find(peerId);
    if (it == sessions_.end()) return false;

    const Entry& entry = it->second;
    const bool failed = entry.state == SessionState::Failed;
    const bool stalled = entry.state == SessionState::Handshaking
                         && clock_.now() - entry.startedAt > protocol::kHandshakeTimeoutMillis;

    if (!failed && !stalled) return false;

    AT_LOG_DEBUG("session", "dropping {} handshake with {} so it can start over",
                 failed ? "failed" : "stalled", peerId.toHex());
    sessions_.erase(it);
    return true;
}

Outcome<PublicKey> SessionManager::remoteStaticKey(const PeerId& peerId) const
{
    const auto it = sessions_.find(peerId);
    if (it == sessions_.end() || !it->second.session) {
        return Outcome<PublicKey>::error(ErrorCode::NotFound, "No session with that peer.");
    }
    return it->second.session->remoteStaticKey();
}

Result SessionManager::encrypt(const PeerId& peerId, ByteSpan plaintext, ByteBuffer& out)
{
    const auto it = sessions_.find(peerId);
    if (it == sessions_.end() || it->second.state != SessionState::Established) {
        return Result::error(ErrorCode::PreconditionFailed, "No established session.");
    }
    return it->second.session->encrypt(plaintext, out);
}

Result SessionManager::decrypt(const PeerId& peerId, ByteSpan ciphertext, ByteBuffer& out)
{
    const auto it = sessions_.find(peerId);
    if (it == sessions_.end() || it->second.state != SessionState::Established) {
        return Result::error(ErrorCode::PreconditionFailed, "No established session.");
    }
    return it->second.session->decrypt(ciphertext, out);
}

void SessionManager::forget(const PeerId& peerId)
{
    if (sessions_.erase(peerId) > 0) {
        AT_LOG_DEBUG("session", "session with {} dropped", peerId.toHex());
    }
}

void SessionManager::forgetAll()
{
    sessions_.clear();
}

}  // namespace anytime
