// core/domain/Entities.cpp

#include "core/domain/Entities.h"

namespace anytime {

namespace {

/// "@vinay·4f2a" — the username plus a fingerprint discriminator.
///
/// AD-05: usernames are display labels and may collide, because with no server
/// there is no registry to make them unique. The suffix is what lets two people
/// called "vinay" be told apart in the Nearby list. It is never an identifier.
std::string handleFor(const std::string& username, const PeerId& peerId)
{
    if (username.empty()) return "@" + peerId.shortLabel();
    return "@" + username + "·" + peerId.shortLabel();
}

}  // namespace

std::string Account::displayHandle() const
{
    return handleFor(username, peerId);
}

std::string Peer::displayHandle() const
{
    return handleFor(nickname, peerId);
}

bool Peer::isReachable(Timestamp now) const
{
    // Liveness comes from announces, never from link events (§11). BLE
    // connection callbacks are unreliable on every platform, so a peer counts as
    // present only while a signature-verified ANNOUNCE sits inside the window.
    if (verifiedAnnounceAt == kNoTimestamp) return false;
    return (now - verifiedAnnounceAt) <= kPeerReachableWindow;
}

}  // namespace anytime
