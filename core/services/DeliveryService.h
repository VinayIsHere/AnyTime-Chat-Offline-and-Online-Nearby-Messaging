// core/services/DeliveryService.h
//
// The outbox and the retry ladder (§09).
//
// This is what makes the app usable when someone drifts in and out of range.
// Messages persist across app kill and reboot; when DiscoveryService reports a
// peer with pending mail is reachable again, its queue drains in order.
//
//   queued ──> sent ──> delivered ──> read
//     ^          |
//     └──────────┘  no ACK in window: requeue, attempt+1
//                |
//                └──> failed  (8 attempts, or 24 h elapsed)

#pragma once

#include <vector>

#include "core/domain/Entities.h"
#include "core/domain/Result.h"
#include "core/ports/IClock.h"
#include "core/protocol/AppEnvelope.h"

namespace anytime {

class IOutbox;
class IMessageRepository;
class IEventSink;
class IClock;
class MeshEngine;
class DiscoveryService;

class DeliveryService {
public:
    DeliveryService(MeshEngine&, DiscoveryService&, IOutbox&, IMessageRepository&,
                    IEventSink&, IClock&);
    ~DeliveryService();

    void start();
    void stop();

    /// Queues an envelope for a peer and attempts an immediate send. The message
    /// row is already stored by ChatService; this owns only its delivery.
    Result enqueue(const MessageId&, const PeerId&, const protocol::AppEnvelope&);

    /// Called by DiscoveryService when a peer re-enters the reachability window.
    void onPeerReachable(const PeerId&);

    /// Called by MeshEngine on an inbound ACK — clears the outbox entry and
    /// flips the message to Delivered.
    void onAckReceived(const PeerId&, const ClientMessageId&);

    /// Called when a Noise session completes, since sends queued before the
    /// handshake finished can now go.
    void onSessionEstablished(const PeerId&);

    /// Manual retry from the UI after a message reached Failed.
    Result retry(const MessageId&);

private:
    /// Timer-driven: sends everything due, then reaps expired entries and flips
    /// their messages to Failed.
    void tick();

    void drainPeer(const PeerId&);
    void setStatus(const MessageId&, DeliveryStatus);

    static Timestamp backoffFor(int attempt);

    MeshEngine&         mesh_;
    DiscoveryService&   discovery_;
    IOutbox&            outbox_;
    IMessageRepository& messages_;
    IEventSink&         events_;
    IClock&             clock_;

    TimerId tickTimer_ = kInvalidTimer;
    bool    running_   = false;
};

}  // namespace anytime
