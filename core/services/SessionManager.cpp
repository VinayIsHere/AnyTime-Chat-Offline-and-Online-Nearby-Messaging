// core/services/SessionManager.cpp
//
// Session bookkeeping. The Noise handshake itself lands with the messaging
// milestone (AD-06), but the keying discipline is enforced from day one:
// sessions are keyed on PeerId, never LinkId — see the header for why that
// matters to the phase 5 transport handoff.

#include "core/services/SessionManager.h"

#include "core/ports/IClock.h"

namespace anytime {

SessionManager::SessionManager(ISecureSessionFactory& factory, IClock& clock)
    : factory_(factory), clock_(clock)
{
}

SessionManager::~SessionManager() = default;

SessionState SessionManager::stateFor(const PeerId& peerId) const
{
    const auto it = sessions_.find(peerId);
    return it == sessions_.end() ? SessionState::None : it->second.state;
}

Result SessionManager::beginHandshake(const PeerId& peerId, ByteBuffer& out)
{
    Entry& entry = sessions_[peerId];
    if (entry.state == SessionState::Established) return Result::ok();
    if (entry.state == SessionState::Handshaking) return Result::ok();

    auto created = factory_.createSession(nullptr);
    if (created.isError()) return created.result();

    entry.session = std::move(created).value();
    entry.startedAt = clock_.now();

    const Result r = entry.session->beginHandshake(HandshakeRole::Initiator, out);
    entry.state = r.isOk() ? SessionState::Handshaking : SessionState::Failed;
    return r;
}

Result SessionManager::readHandshake(const PeerId& peerId, ByteSpan message, ByteBuffer& out)
{
    Entry& entry = sessions_[peerId];

    if (!entry.session) {
        auto created = factory_.createSession(nullptr);
        if (created.isError()) return created.result();
        entry.session = std::move(created).value();
        entry.startedAt = clock_.now();
        entry.state = SessionState::Handshaking;

        ByteBuffer ignored;
        if (const Result r = entry.session->beginHandshake(HandshakeRole::Responder, ignored);
            r.isError()) {
            entry.state = SessionState::Failed;
            return r;
        }
    }

    const Result r = entry.session->readHandshake(message, out);
    if (r.isError()) {
        entry.state = SessionState::Failed;
        return r;
    }
    if (entry.session->isEstablished()) entry.state = SessionState::Established;
    return Result::ok();
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

void SessionManager::forget(const PeerId& peerId) { sessions_.erase(peerId); }

void SessionManager::forgetAll() { sessions_.clear(); }

}  // namespace anytime
