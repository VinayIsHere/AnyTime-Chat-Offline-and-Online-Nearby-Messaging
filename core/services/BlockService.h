// core/services/BlockService.h
//
// The block list as a user manages it: block someone from a chat or a request,
// see who is blocked, and take it back.
//
// ENFORCEMENT DOES NOT LIVE HERE. Every service that could let a blocked peer
// through already checks IContactRepository::isBlocked() at its own door —
// DiscoveryService drops their announces, RequestService drops their requests,
// ChatService drops their messages and refuses ours. That stays true however a
// block was made, including decline-with-block, which never comes through here.
// This class only changes the list and tidies what a new block leaves behind.
//
// Separate from RequestService because blocking an existing CONTACT has nothing
// to do with requests, and so this can be tested without a mesh.

#pragma once

#include <vector>

#include "core/domain/Entities.h"
#include "core/domain/Result.h"

namespace anytime {

class IContactRepository;
class IEventSink;
class AccountService;

class BlockService {
public:
    BlockService(IContactRepository&, AccountService&, IEventSink&);

    /// POST /{t}/api/blocked — idempotent.
    ///
    /// Also declines anything they have waiting in the Requests tab: leaving a
    /// blocked person's request there to be accepted by accident would undo the
    /// block with one tap. Nothing is sent to them — telling someone they were
    /// blocked is itself information (§08).
    Result block(const PeerId&);

    /// DELETE /{t}/api/blocked/:peerId — idempotent. A former contact's chat
    /// works again straight away; the pinned key was never touched.
    Result unblock(const PeerId&);

    /// GET /{t}/api/blocked — most recent first.
    Outcome<std::vector<BlockedPeer>> list();

private:
    IContactRepository& contacts_;
    AccountService&     accounts_;
    IEventSink&         events_;
};

}  // namespace anytime
