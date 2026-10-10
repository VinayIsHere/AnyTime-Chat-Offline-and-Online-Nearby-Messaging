// adapters/events/FanOutEventSink.h
//
// One IEventSink in front of several. The core emits each domain event exactly
// once, to exactly one sink (AppCoreDeps::events) — which is right, the core
// should not know who is listening. This is how more than one party listens:
// the SSE hub for the open page, and the notifier for a user who has left it.
//
// Order is the order given. Every sink sees every event; one sink cannot stop
// another from hearing it.

#pragma once

#include <initializer_list>
#include <string>
#include <vector>

#include "core/ports/IEventSink.h"

namespace anytime::events {

class FanOutEventSink final : public IEventSink {
public:
    FanOutEventSink(std::initializer_list<IEventSink*> sinks)
    {
        for (IEventSink* sink : sinks) {
            if (sink != nullptr) sinks_.push_back(sink);
        }
    }

    void onPeerDiscovered(const Peer& p) override { for (auto* s : sinks_) s->onPeerDiscovered(p); }
    void onPeerLost(const PeerId& p) override { for (auto* s : sinks_) s->onPeerLost(p); }

    void onRequestReceived(const Request& r) override { for (auto* s : sinks_) s->onRequestReceived(r); }
    void onRequestAccepted(const Request& r, const Chat& c) override
    {
        for (auto* s : sinks_) s->onRequestAccepted(r, c);
    }
    void onRequestDeclined(const RequestId& id, const PeerId& p) override
    {
        for (auto* s : sinks_) s->onRequestDeclined(id, p);
    }

    void onMessageReceived(const Message& m) override { for (auto* s : sinks_) s->onMessageReceived(m); }
    void onMessageStatusChanged(const MessageId& id, DeliveryStatus st) override
    {
        for (auto* s : sinks_) s->onMessageStatusChanged(id, st);
    }

    void onRadioStateChanged(bool available, const std::string& detail) override
    {
        for (auto* s : sinks_) s->onRadioStateChanged(available, detail);
    }

private:
    std::vector<IEventSink*> sinks_;
};

}  // namespace anytime::events
