// core/AppCore.cpp
//
// Composition root for the core. Owns every service and wires the listener
// chain described in the header.

#include "core/AppCore.h"

#include "core/ports/IClock.h"
#include "core/ports/IEventSink.h"
#include "core/ports/IIdentityStore.h"
#include "core/ports/IRepositories.h"
#include "core/ports/ISecureSession.h"
#include "core/ports/ITransport.h"
#include "core/services/AccountService.h"
#include "core/services/DiscoveryService.h"
#include "core/services/RoutingPolicy.h"
#include "core/services/SessionManager.h"
#include "core/services/TransportRegistry.h"

namespace anytime {

struct AppCore::Impl {
    AppCoreDeps deps;

    TransportRegistry registry;
    RoutingPolicy     routing{registry};
    SessionManager    sessions;
    MeshEngine        mesh;
    AccountService    accounts;
    DiscoveryService  discovery;

    Impl(const AppCoreDeps& d, AppCore& owner)
        : deps(d),
          sessions(*d.sessions, *d.clock),
          mesh(registry, sessions, routing, *d.identity, *d.cbor, *d.clock),
          accounts(*d.accounts, *d.identity, *d.clock),
          discovery(registry, mesh, *d.contacts, *d.events, *d.clock)
    {
        // Transports report to the registry, the registry to the mesh, the mesh
        // to AppCore, and AppCore to the services. One direction throughout.
        registry.setListener(&mesh);
        mesh.setListener(&owner);

        // DiscoveryService needs raw transport sightings for link handles and
        // radio state, but MeshEngine must not depend on it — hooks keep the
        // dependency pointing one way.
        mesh.setPeerSeenHook([this](TransportId, const PeerHandle& handle) {
            discovery.rememberHandle(handle);
        });
        mesh.setRadioStateHook([this](bool available) {
            deps.events->onRadioStateChanged(available, available ? "" : "transport_down");
        });
    }
};

// ─────────────────────────────────────────────────────────────────────────────

AppCore::AppCore(const AppCoreDeps& deps) : impl_(std::make_unique<Impl>(deps, *this)) {}

AppCore::~AppCore() = default;

void AppCore::addTransport(std::unique_ptr<ITransport> transport)
{
    impl_->registry.addTransport(std::move(transport));
}

AccountService&   AppCore::accounts()  { return impl_->accounts; }
DiscoveryService& AppCore::discovery() { return impl_->discovery; }

Result AppCore::start()
{
    auto account = impl_->accounts.current();
    if (account.isError()) return account.result();

    // No account yet: the shell still runs, the radio simply stays down until
    // setup completes. The UI routes to /welcome on the matching 404.
    if (!account.value().has_value()) return Result::ok();

    return bringUp(*account.value());
}

Result AppCore::startAfterAccountCreated()
{
    auto account = impl_->accounts.current();
    if (account.isError()) return account.result();
    if (!account.value().has_value()) {
        return Result::error(ErrorCode::PreconditionFailed, "No account to start with.");
    }
    return bringUp(*account.value());
}

Result AppCore::bringUp(const Account& account)
{
    if (const Result r = impl_->registry.startAll(account.peerId); r.isError()) return r;
    if (const Result r = impl_->mesh.start(account); r.isError()) return r;
    return impl_->registry.setAdvertising(account.discoverable);
}

void AppCore::stop()
{
    impl_->discovery.stop();
    impl_->mesh.stop();
    impl_->registry.stopAll();
}

// ── IMeshListener ────────────────────────────────────────────────────────────

void AppCore::onAnnounce(const PeerId& peerId, const std::string& nickname,
                         const PublicKey& staticKey, int rssi)
{
    impl_->discovery.onAnnounce(peerId, nickname, staticKey, rssi);
}

void AppCore::onEnvelope(const PeerId&, const protocol::AppEnvelope&)
{
    // Routed to RequestService and ChatService in the messaging milestone.
}

void AppCore::onAck(const PeerId&, const ClientMessageId&) {}

void AppCore::onSessionEstablished(const PeerId&) {}

}  // namespace anytime
