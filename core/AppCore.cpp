// core/AppCore.cpp
//
// Composition root for the core. Owns every service and wires the listener
// chain described in the header.

#include "core/AppCore.h"

#include "core/log/Log.h"
#include "core/ports/IClock.h"
#include "core/ports/IEventSink.h"
#include "core/ports/IIdentityStore.h"
#include "core/ports/IRepositories.h"
#include "core/ports/ISecureSession.h"
#include "core/ports/ITransport.h"
#include "core/services/AccountService.h"
#include "core/services/BlockService.h"
#include "core/services/ChatService.h"
#include "core/services/DeliveryService.h"
#include "core/services/DiscoveryService.h"
#include "core/services/RequestService.h"
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
    DeliveryService   delivery;
    RequestService    requests;
    ChatService       chats;
    BlockService      blocks;

    /// True between bringUp() and stop(). Transport events outside that window
    /// have nothing to resume.
    bool   up = false;
    PeerId localPeerId;

    /// Runs while any transport is down. See kTransportRetryMillis for why this
    /// has to exist: on Android the first start() always fails, because the
    /// permission dialog is still on screen.
    TimerId transportRetryTimer = kInvalidTimer;

    Impl(const AppCoreDeps& d, AppCore& owner)
        : deps(d),
          sessions(*d.sessions, *d.clock),
          mesh(registry, sessions, routing, *d.identity, *d.cbor, *d.clock),
          accounts(*d.accounts, *d.identity, *d.clock),
          discovery(registry, mesh, *d.contacts, *d.events, *d.clock),
          delivery(mesh, discovery, *d.outbox, *d.messages, *d.cbor, *d.events, *d.clock),
          requests(mesh, sessions, discovery, accounts, *d.contacts, *d.messages,
                   *d.transactions, *d.events, *d.clock),
          chats(*d.messages, *d.contacts, delivery, mesh, *d.events, *d.clock),
          blocks(*d.contacts, accounts, *d.events)
    {
        // Transports report to the registry, the registry to the mesh, the mesh
        // to AppCore, and AppCore to the services. One direction throughout.
        registry.setListener(&mesh);
        mesh.setListener(&owner);

        // DiscoveryService needs raw sightings — to remember handles and to
        // decide when to open links — but MeshEngine must not depend on it.
        // Hooks keep the dependency pointing one way.
        mesh.setPeerSeenHook([this](TransportId transport, const PeerHandle& handle) {
            discovery.onPeerSeen(transport, handle);
        });
        mesh.setRadioStateHook([this](bool available) { onRadioStateChanged(available); });
    }

    ~Impl() { cancelTransportRetry(); }

    /// Advertising follows the account's discoverable flag. Read fresh each
    /// time, so a settings change made while the radio was down still applies.
    void applyAdvertising()
    {
        auto account = accounts.current();
        if (account.isError() || !account.value().has_value()) return;

        const Result r = registry.setAdvertising(account.value()->discoverable);
        if (r.isError()) AT_LOG_DEBUG("core", "advertising not applied yet: {}", r.message());
    }

    void onRadioStateChanged(bool available)
    {
        deps.events->onRadioStateChanged(available, available ? "" : "transport_down");
        if (!up) return;

        if (available) {
            // Availability can arrive after start() returned, so advertising and
            // scanning requested in the meantime are applied here rather than lost.
            applyAdvertising();
            discovery.resume();
            if (registry.allAvailable()) cancelTransportRetry();
        } else {
            AT_LOG_WARN("core", "a transport went down; retrying every {} ms",
                        kTransportRetryMillis);
            startTransportRetry();
        }
    }

    void startTransportRetry()
    {
        if (transportRetryTimer != kInvalidTimer) return;
        transportRetryTimer = deps.clock->scheduleRepeating(kTransportRetryMillis,
                                                            [this] { retryTransports(); });
    }

    void cancelTransportRetry()
    {
        if (transportRetryTimer == kInvalidTimer) return;
        deps.clock->cancel(transportRetryTimer);
        transportRetryTimer = kInvalidTimer;
    }

    void retryTransports()
    {
        if (!up) return;
        if (registry.allAvailable()) {
            cancelTransportRetry();
            return;
        }

        const Result r = registry.startAll(localPeerId);
        if (r.isError()) return;  // the transport logs why; this runs every few seconds

        applyAdvertising();
        discovery.resume();

        if (registry.allAvailable()) {
            AT_LOG_INFO("core", "all transports are up");
            cancelTransportRetry();
        }
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
RequestService&   AppCore::requests()  { return impl_->requests; }
ChatService&      AppCore::chats()     { return impl_->chats; }
DeliveryService&  AppCore::delivery()  { return impl_->delivery; }
BlockService&     AppCore::blocks()    { return impl_->blocks; }
IRadioController* AppCore::radio()     { return impl_->deps.radio; }

Result AppCore::start()
{
    auto account = impl_->accounts.current();
    if (account.isError()) {
        AT_LOG_ERROR("core", "could not load the account: {}", account.result().message());
        return account.result();
    }

    // No account yet: the shell still runs, the radio simply stays down until
    // setup completes. The UI routes to /welcome on the matching 404.
    if (!account.value().has_value()) {
        AT_LOG_INFO("core", "no account yet; the mesh starts after sign-up");
        return Result::ok();
    }

    return bringUp(*account.value());
}

Outcome<Account> AppCore::updateAccount(const UpdateAccountRequest& request)
{
    auto updated = impl_->accounts.update(request);
    if (updated.isError()) return updated;

    // Saving the row is only half of it. The mesh copies the nickname and the
    // discoverable flag when it starts, and the radio's advertising follows the
    // flag — so without this, a rename or "hide me" was stored but not acted on
    // until the next radio event or restart, while the Profile screen claimed
    // it had taken effect.
    if (impl_->up) {
        impl_->mesh.updateAnnounceContent(updated.value().username, updated.value().discoverable);
        impl_->applyAdvertising();
    }
    return updated;
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
    AT_LOG_INFO("core", "bringing the mesh up as {}", account.peerId.toHex());

    impl_->localPeerId = account.peerId;
    impl_->sessions.setLocalPeerId(account.peerId);
    impl_->discovery.setLocalPeerId(account.peerId);

    // A transport that fails to start is NOT fatal. On Android the first attempt
    // always fails while the permission dialog is on screen, and Bluetooth may
    // simply be switched off. Returning here used to skip mesh.start() as well,
    // so the app came up with announces disabled and never recovered.
    const Result started = impl_->registry.startAll(account.peerId);
    if (started.isError()) {
        AT_LOG_WARN("core", "no transport running yet ({}); retrying every {} ms",
                    started.message(), kTransportRetryMillis);
    }

    if (const Result r = impl_->mesh.start(account); r.isError()) {
        AT_LOG_ERROR("core", "mesh failed to start: {}", r.message());
        return r;
    }

    impl_->delivery.start();
    impl_->up = true;

    impl_->applyAdvertising();
    if (!impl_->registry.allAvailable()) impl_->startTransportRetry();

    return Result::ok();
}

void AppCore::stop()
{
    AT_LOG_INFO("core", "stopping");

    impl_->up = false;
    impl_->cancelTransportRetry();
    impl_->delivery.stop();
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

void AppCore::onEnvelope(const PeerId& peerId, const protocol::AppEnvelope& envelope)
{
    // Type and sender only. The envelope content is private — see Log.h.
    AT_LOG_DEBUG("core", "envelope type {} from {}", static_cast<int>(envelope.type),
                 peerId.toHex());

    // This switch is the whole application protocol. Everything below it has
    // already been authenticated and decrypted by MeshEngine.
    switch (envelope.type) {
        case protocol::EnvelopeType::ChatRequest:
            if (envelope.request) {
                impl_->requests.onRequestReceived(peerId, envelope.request->nickname,
                                                  envelope.request->greeting);
            }
            break;

        case protocol::EnvelopeType::ChatAccept:
            impl_->requests.onAcceptReceived(
                peerId, envelope.accept ? envelope.accept->nickname : std::string());
            break;

        case protocol::EnvelopeType::ChatDecline:
            impl_->requests.onDeclineReceived(peerId);
            break;

        case protocol::EnvelopeType::ChatMessage:
            if (envelope.message) impl_->chats.onMessageReceived(peerId, *envelope.message);
            break;

        case protocol::EnvelopeType::ChatRead:
            if (envelope.read) {
                impl_->chats.onReadReceipt(peerId, envelope.read->clientMessageId);
            }
            break;

        case protocol::EnvelopeType::ChatMedia:
            if (envelope.media) impl_->chats.onMediaReceived(peerId, *envelope.media);
            break;
    }
}

void AppCore::onAck(const PeerId& peerId, const ClientMessageId& clientMessageId)
{
    impl_->delivery.onAckReceived(peerId, clientMessageId);
}

void AppCore::onSessionEstablished(const PeerId& peerId)
{
    // Anything queued while the handshake was in flight can go now.
    impl_->delivery.onSessionEstablished(peerId);
}

}  // namespace anytime
