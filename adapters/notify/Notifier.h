// adapters/notify/Notifier.h
//
// Tells someone who has LEFT the app that a message or a chat request arrived.
//
// It listens to the same domain events the SSE hub does (see FanOutEventSink),
// so the core did not change to support it: the core already says "a message
// arrived", and turning that into a system notification is presentation, like
// turning it into an SSE frame is.
//
// Split in two so the decisions are testable without a phone:
//   Notifier                 — what to say: whose name, what text, where a tap
//                              goes. Plain C++, unit tested on desktop.
//   INotificationPresenter   — putting it on screen. AndroidNotificationPresenter
//                              is a thin JNI call; whether the app is in the
//                              foreground, and whether notifications are
//                              permitted at all, are decided there, by the OS.
//
// Logs nothing about content. A notification carries a message body, and the
// log policy (core/log/Log.h) says bodies are never logged.

#pragma once

#include <string>

#include "core/ports/IEventSink.h"
#include "core/ports/IRepositories.h"

namespace anytime::notify {

enum class NoticeKind { Message, Request };

/// One notification, fully decided.
struct Notice {
    NoticeKind  kind = NoticeKind::Message;

    /// Groups notifications: every message in one chat updates the same entry,
    /// and all requests share one. A chat id, or "requests".
    std::string thread;

    std::string title;
    std::string text;

    /// Where a tap should land, relative to the app's base URL — "chats/<id>"
    /// or "requests". The platform side validates it again before using it.
    std::string route;
};

class INotificationPresenter {
public:
    virtual ~INotificationPresenter() = default;
    virtual void show(const Notice&) = 0;
};

/// Longest notification text, in bytes. A notification shows two lines; the rest
/// belongs in the app, and a 2000-character message crossing JNI for nothing is
/// waste. Cut on a UTF-8 boundary, never inside a character.
inline constexpr std::size_t kMaxNoticeTextBytes = 240;

class Notifier final : public IEventSink {
public:
    Notifier(IMessageRepository&, IContactRepository&, INotificationPresenter&);

    void onMessageReceived(const Message&) override;
    void onRequestReceived(const Request&) override;

    // Nothing else is worth interrupting someone for.
    void onPeerDiscovered(const Peer&) override {}
    void onPeerLost(const PeerId&) override {}
    void onRequestAccepted(const Request&, const Chat&) override {}
    void onRequestDeclined(const RequestId&, const PeerId&) override {}
    void onMessageStatusChanged(const MessageId&, DeliveryStatus) override {}
    void onRadioStateChanged(bool, const std::string&) override {}

private:
    IMessageRepository&     messages_;
    IContactRepository&     contacts_;
    INotificationPresenter& presenter_;
};

/// Cuts `text` to at most `maxBytes`, backing off to a UTF-8 character boundary
/// and adding "…" when anything was removed. Exposed for tests.
std::string clipForNotice(const std::string& text, std::size_t maxBytes = kMaxNoticeTextBytes);

}  // namespace anytime::notify
