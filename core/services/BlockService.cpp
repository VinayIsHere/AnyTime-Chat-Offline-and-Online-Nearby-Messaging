// core/services/BlockService.cpp
//
// Logs carry peer ids only — never nicknames.

#include "core/services/BlockService.h"

#include "core/log/Log.h"
#include "core/ports/IEventSink.h"
#include "core/ports/IRepositories.h"
#include "core/services/AccountService.h"

namespace anytime {

BlockService::BlockService(IContactRepository& contacts, AccountService& accounts,
                           IEventSink& events)
    : contacts_(contacts), accounts_(accounts), events_(events)
{
}

Result BlockService::block(const PeerId& peerId)
{
    // Blocking ourselves would drop our own announces coming back over a second
    // transport, which is harmless, but it would also sit in the list forever
    // with an Unblock button that means nothing. Refuse it at the door.
    auto account = accounts_.current();
    if (account.isOk() && account.value().has_value() && account.value()->peerId == peerId) {
        return Result::error(ErrorCode::InvalidArgument, "You cannot block yourself.");
    }

    if (const Result r = contacts_.setBlocked(peerId, true); r.isError()) {
        AT_LOG_ERROR("block", "could not block {}: {}", peerId.toHex(), r.message());
        return r;
    }

    auto pending = contacts_.listRequests(Direction::Incoming, RequestState::Pending);
    if (pending.isOk()) {
        for (const Request& request : pending.value()) {
            if (request.peerId != peerId) continue;
            if (const Result r = contacts_.setRequestState(request.id, RequestState::Declined);
                r.isError()) {
                // Not fatal: RequestService refuses to accept from a blocked
                // peer anyway, so a request left behind here cannot undo it.
                AT_LOG_WARN("block", "request {} from {} left pending: {}", request.id.value(),
                            peerId.toHex(), r.message());
            }
        }
    }

    // DiscoveryService stops reporting them from now on, but a peer already on
    // the Nearby screen would otherwise stay there until their 60 s window ran
    // out. Saying they are gone is true from where the user is standing.
    events_.onPeerLost(peerId);

    AT_LOG_INFO("block", "blocked {}", peerId.toHex());
    return Result::ok();
}

Result BlockService::unblock(const PeerId& peerId)
{
    if (const Result r = contacts_.setBlocked(peerId, false); r.isError()) {
        AT_LOG_ERROR("block", "could not unblock {}: {}", peerId.toHex(), r.message());
        return r;
    }
    AT_LOG_INFO("block", "unblocked {}", peerId.toHex());
    return Result::ok();
}

Outcome<std::vector<BlockedPeer>> BlockService::list()
{
    return contacts_.listBlocked();
}

}  // namespace anytime
