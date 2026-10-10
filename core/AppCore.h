// core/AppCore.h
//
// Composition root and the single façade the HTTP adapter drives.
//
// AppCore owns every service and wires the listener chain:
//
//   TransportRegistry ──> MeshEngine ──> AppCore ──> Discovery/Request/Chat/Delivery
//        (ITransportListener)  (IMeshListener)
//
// The HTTP adapter holds a reference to AppCore and calls the services through
// it. It never sees a transport, a session or a repository.
//
// THREADING: every public method here must be called on the core thread. The
// HTTP adapter marshals from its socket thread; BleGattTransport marshals from
// binder threads (§11). There is no lock inside the core, by design.

#pragma once

#include <memory>

#include "core/domain/Result.h"
#include "core/services/MeshEngine.h"

namespace anytime {

class IAccountRepository;
class IContactRepository;
class IMessageRepository;
class IOutbox;
class ITransactionScope;
class IIdentityStore;
class ISecureSessionFactory;
class IEventSink;
class IClock;
class ITransport;
class IRadioController;

class AccountService;
class BlockService;
struct UpdateAccountRequest;
class ChatService;
class DeliveryService;
class DiscoveryService;
class RequestService;
class RoutingPolicy;
class SessionManager;
class TransportRegistry;

/// Everything AppCore needs, supplied by the platform's main(). Adapters are
/// borrowed references — main() owns them and outlives AppCore.
struct AppCoreDeps {
    IAccountRepository*    accounts   = nullptr;
    IContactRepository*    contacts   = nullptr;
    IMessageRepository*    messages   = nullptr;
    IOutbox*               outbox     = nullptr;

    /// Must be the same object as `contacts` and `messages`: a transaction
    /// only spans repositories that share one connection.
    ITransactionScope*     transactions = nullptr;
    IIdentityStore*        identity   = nullptr;
    ISecureSessionFactory* sessions   = nullptr;
    protocol::ICborCodec*  cbor       = nullptr;
    IEventSink*            events     = nullptr;
    IClock*                clock      = nullptr;

    /// OPTIONAL, and null is a real answer rather than a missing dependency:
    /// desktop has no radio to switch on (AD-07). The HTTP layer reports
    /// "unsupported" in that case, which the UI reads as "nothing to gate on".
    IRadioController*      radio      = nullptr;
};

class AppCore : public IMeshListener {
public:
    explicit AppCore(const AppCoreDeps&);
    ~AppCore() override;

    AppCore(const AppCore&)            = delete;
    AppCore& operator=(const AppCore&) = delete;

    /// Register transports before start(). v1 registers LoopbackTransport on
    /// desktop and BleGattTransport on Android (AD-07: loopback is a shipped
    /// adapter, not a test double).
    void addTransport(std::unique_ptr<ITransport>);

    /// Loads the account if one exists and brings the mesh up. Safe to call
    /// before setup — the radio simply stays down until an account is created.
    Result start();
    void   stop();

    /// Brings the radio up immediately after POST /account succeeds, so the
    /// Nearby screen is live by the time the UI navigates to it.
    Result startAfterAccountCreated();

    /// PATCH /{t}/api/account. Use this rather than accounts().update(): it also
    /// hands a new nickname or discoverable flag to the running mesh, which
    /// otherwise keeps announcing whatever it was started with.
    Outcome<Account> updateAccount(const UpdateAccountRequest&);

    // ── service accessors for the HTTP adapter ───────────────────────────────
    AccountService&   accounts();
    DiscoveryService& discovery();
    RequestService&   requests();
    ChatService&      chats();
    DeliveryService&  delivery();
    BlockService&     blocks();

    /// Null where the platform has no radio. Callers must handle that rather
    /// than assume — see AppCoreDeps::radio.
    IRadioController* radio();

    // ── IMeshListener — routes decrypted traffic to the right service ────────
    void onAnnounce(const PeerId&, const std::string& nickname,
                    const PublicKey&, int rssi) override;
    void onEnvelope(const PeerId&, const protocol::AppEnvelope&) override;
    void onAck(const PeerId&, const ClientMessageId&) override;
    void onSessionEstablished(const PeerId&) override;

private:
    Result bringUp(const Account&);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace anytime
