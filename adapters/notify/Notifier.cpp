// adapters/notify/Notifier.cpp

#include "adapters/notify/Notifier.h"

namespace anytime::notify {

namespace {

/// "@vinay·4f2a". Same shape as everywhere else in the app, so the name in a
/// notification matches the name in the chat it opens.
std::string handleFor(const std::string& name, const PeerId& peerId)
{
    if (name.empty()) return "@" + peerId.shortLabel();
    return "@" + name + "\xC2\xB7" + peerId.shortLabel();  // U+00B7 middle dot
}

}  // namespace

std::string clipForNotice(const std::string& text, std::size_t maxBytes)
{
    if (text.size() <= maxBytes) return text;

    static const std::string kEllipsis = "\xE2\x80\xA6";  // U+2026
    std::size_t cut = maxBytes > kEllipsis.size() ? maxBytes - kEllipsis.size() : 0;

    // Step back off any UTF-8 continuation byte (10xxxxxx), so the cut lands at
    // the start of a character rather than through the middle of one — a split
    // emoji would reach Java as invalid modified-UTF-8.
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
    return text.substr(0, cut) + kEllipsis;
}

Notifier::Notifier(IMessageRepository& messages, IContactRepository& contacts,
                   INotificationPresenter& presenter)
    : messages_(messages), contacts_(contacts), presenter_(presenter)
{
}

void Notifier::onMessageReceived(const Message& message)
{
    // Only messages TO us. The event is inbound-only today; checking keeps a
    // future caller from making the phone notify its owner of their own words.
    if (message.isMine()) return;

    auto chat = messages_.findChat(message.chatId);
    if (chat.isError() || !chat.value().has_value()) return;
    const PeerId peerId = chat.value()->peerId;

    // The PINNED contact name, never the peer's live announced nickname: a
    // notification is exactly where an impersonator would want to borrow a
    // trusted name, and the pinned one is the one that was authenticated.
    std::string name;
    auto contact = contacts_.findContact(peerId);
    if (contact.isOk() && contact.value().has_value()) name = contact.value()->displayName;

    Notice notice;
    notice.kind   = NoticeKind::Message;
    notice.thread = message.chatId.value();
    notice.title  = handleFor(name, peerId);
    notice.route  = "chats/" + message.chatId.value();

    if (message.isMedia()) {
        notice.text = message.body.empty() ? std::string("Photo") : "Photo \xC2\xB7 " + message.body;
    } else {
        notice.text = message.body;
    }
    notice.text = clipForNotice(notice.text);

    presenter_.show(notice);
}

void Notifier::onRequestReceived(const Request& request)
{
    if (request.direction != Direction::Incoming) return;

    const std::string who = handleFor(request.nickname, request.peerId);

    Notice notice;
    notice.kind   = NoticeKind::Request;
    notice.thread = "requests";
    notice.title  = "New chat request";
    notice.text   = clipForNotice(request.greeting.empty() ? who + " wants to chat"
                                                           : who + ": " + request.greeting);
    notice.route  = "requests";

    presenter_.show(notice);
}

}  // namespace anytime::notify
