// tests/PeerPresentationTests.cpp
//
// How the Nearby screen presents a peer, and in particular the resend cooldown.
//
// WHY THIS IS A PURE FUNCTION. The decision depends on three facts — are they a
// contact, what state is the newest outgoing request in, and how old is it —
// and on nothing else. Keeping it free of the service means the cooldown
// boundary can be tested exactly, without a MeshEngine, a SessionManager and
// three repositories standing behind it.
//
// The boundary is the point. Before the cooldown existed, an unanswered request
// left the button disabled forever: the peer ignored it, or walked out of range,
// or was killed before it arrived, and the user could never ask again.

#include "tests/TestHarness.h"

#include "core/services/RequestService.h"

namespace anytime::test {

namespace {

constexpr Timestamp kCooldown = kRequestResendCooldownMillis;  // 60 s

}  // namespace

void runPeerPresentationTests(Registry& r)
{
    r.section("Peer presentation (Nearby button state)");

    // ── never asked ─────────────────────────────────────────────────────────

    {
        const PeerPresentation view = presentPeer(false, std::nullopt, 0, kCooldown);
        r.check(view.status == PeerStatus::None, "a stranger shows as none");
        r.check(view.canResend, "a stranger can be greeted");
    }

    // ── request sent, still waiting ─────────────────────────────────────────

    {
        const PeerPresentation fresh =
            presentPeer(false, RequestState::Pending, 0, kCooldown);
        r.check(fresh.status == PeerStatus::Requested, "a fresh request shows as requested");
        r.check(!fresh.canResend, "a fresh request cannot be resent immediately");

        const PeerPresentation almost =
            presentPeer(false, RequestState::Pending, kCooldown - 1, kCooldown);
        r.check(almost.status == PeerStatus::Requested, "still requested just under the cooldown");
        r.check(!almost.canResend, "one millisecond under the cooldown is still too soon");

        // THE BOUNDARY.
        const PeerPresentation exactly =
            presentPeer(false, RequestState::Pending, kCooldown, kCooldown);
        r.check(exactly.canResend, "exactly at the cooldown it becomes resendable");

        const PeerPresentation stale =
            presentPeer(false, RequestState::Pending, kCooldown * 10, kCooldown);
        r.check(stale.canResend, "a long-unanswered request is resendable");
        r.check(stale.status == PeerStatus::Requested,
                "and it STILL reads as requested — the user is not told it was never sent");
    }

    // ── they declined ───────────────────────────────────────────────────────

    {
        const PeerPresentation view =
            presentPeer(false, RequestState::Declined, 0, kCooldown);
        r.check(view.status == PeerStatus::Declined, "a decline is surfaced");
        r.check(view.canResend, "a decline can be followed by asking again, with no cooldown");
    }

    // ── already a contact ───────────────────────────────────────────────────

    {
        const PeerPresentation view = presentPeer(true, std::nullopt, 0, kCooldown);
        r.check(view.status == PeerStatus::Contact, "an accepted peer shows as a contact");
        r.check(!view.canResend, "a contact is messaged, not greeted again");
    }

    // Being a contact outranks whatever the old request row says — after an
    // accept the request state is history.
    {
        const PeerPresentation afterPending =
            presentPeer(true, RequestState::Pending, 0, kCooldown);
        r.check(afterPending.status == PeerStatus::Contact,
                "contact wins over a stale pending row");

        const PeerPresentation afterDeclined =
            presentPeer(true, RequestState::Declined, 0, kCooldown);
        r.check(afterDeclined.status == PeerStatus::Contact,
                "contact wins over an old decline, so a re-accept is not shown as declined");
    }

    // ── accepted, but no contact exists ─────────────────────────────────────
    //
    // Reachable when establishContact failed — typically no authenticated
    // session at the time. Stranding the user on a dead state would leave them
    // no way to reach someone standing next to them.

    {
        const PeerPresentation view =
            presentPeer(false, RequestState::Accepted, 0, kCooldown);
        r.check(view.canResend, "an accept that never produced a contact can be retried");
        r.check(view.status == PeerStatus::None, "and it does not claim to be a contact");
    }

    // ── the default cooldown is the shipped one ─────────────────────────────

    {
        r.check(kRequestResendCooldownMillis == 60LL * 1000,
                "the shipped resend cooldown is one minute");

        // Called without the trailing argument, so the default actually binds.
        const PeerPresentation defaulted = presentPeer(false, RequestState::Pending, 0);
        r.check(!defaulted.canResend, "the default cooldown applies when none is passed");
    }
}

}  // namespace anytime::test
