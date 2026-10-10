// adapters/http/ApiRoutes.cpp

#include "adapters/http/ApiRoutes.h"

#include <unordered_set>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "adapters/http/JsonMapping.h"
#include "adapters/http/Router.h"
#include "adapters/http/SseHub.h"
#include "core/AppCore.h"
#include "core/ports/IRadioController.h"
#include "core/services/AccountService.h"
#include "core/services/BlockService.h"
#include "core/services/ChatService.h"
#include "core/services/DeliveryService.h"
#include "core/services/DiscoveryService.h"
#include "core/services/RequestService.h"

namespace anytime::http {

namespace {

/// Maps a core ErrorCode onto the HTTP status the UI expects. Keep this in step
/// with the branches in ui/src/api/client.ts.
int statusFor(ErrorCode code)
{
    switch (code) {
        case ErrorCode::Ok:                   return 200;
        case ErrorCode::NotFound:             return 404;
        case ErrorCode::AlreadyExists:        return 409;
        case ErrorCode::InvalidArgument:      return 400;
        case ErrorCode::PermissionDenied:     return 403;
        case ErrorCode::PreconditionFailed:   return 412;
        case ErrorCode::TransportUnavailable: return 503;
        default:                              return 500;
    }
}

Response fromResult(const Result& r)
{
    return Response::error(statusFor(r.code()), toString(r.code()),
                           QString::fromStdString(r.message()));
}

std::optional<QJsonObject> parseBody(const Request& request)
{
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(request.body, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return std::nullopt;
    return doc.object();
}

QByteArray compact(const QJsonObject& o) { return QJsonDocument(o).toJson(QJsonDocument::Compact); }
QByteArray compact(const QJsonArray& a) { return QJsonDocument(a).toJson(QJsonDocument::Compact); }

/// Content type for image bytes, decided by INSPECTING them rather than by
/// trusting a value the caller supplied.
///
/// This matters more than it looks. The response is rendered inside our own
/// WebView, at the API's own origin. If a caller could name the Content-Type,
/// a stored blob could be served back as text/html and run as script with full
/// access to the local API — the one origin that can read every chat. Sniffing
/// means an unrecognised blob is served as an opaque download instead.
QByteArray sniffImageType(const QByteArray& bytes)
{
    if (bytes.startsWith(QByteArrayLiteral("\xFF\xD8\xFF"))) return "image/jpeg";
    if (bytes.startsWith(QByteArrayLiteral("\x89PNG\r\n\x1A\n"))) return "image/png";
    if (bytes.startsWith(QByteArrayLiteral("GIF87a"))
        || bytes.startsWith(QByteArrayLiteral("GIF89a"))) {
        return "image/gif";
    }
    if (bytes.size() > 12 && bytes.startsWith(QByteArrayLiteral("RIFF"))
        && bytes.mid(8, 4) == QByteArrayLiteral("WEBP")) {
        return "image/webp";
    }
    return "application/octet-stream";
}

/// Peers currently inside the announce window. Reachability is live radio state,
/// so it comes from DiscoveryService rather than from storage — the database has
/// no idea who is in range right now (section 11).
std::unordered_set<PeerId> reachablePeerIds(AppCore& core)
{
    std::unordered_set<PeerId> out;
    auto peers = core.discovery().reachablePeers();
    if (peers.isOk()) {
        for (const Peer& peer : peers.value()) out.insert(peer.peerId);
    }
    return out;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────

void registerApiRoutes(Router& router, AppCore& core, SseHub&)
{
    // ── Account ─────────────────────────────────────────────────────────────

    router.add(Method::Get, QStringLiteral("account"),
               [&core](const Request&, const PathParams&) -> Response {
                   auto outcome = core.accounts().current();
                   if (outcome.isError()) return fromResult(outcome.result());
                   if (!outcome.value().has_value()) {
                       // The expected answer before setup — the UI routes to
                       // /welcome on exactly this.
                       return Response::error(404, "not_found",
                                              QStringLiteral("No account on this device yet."));
                   }
                   return Response::json(200, compact(json::accountToJson(*outcome.value())));
               });

    router.add(Method::Post, QStringLiteral("account"),
               [&core](const Request& request, const PathParams&) -> Response {
                   const auto body = parseBody(request);
                   if (!body) {
                       return Response::error(400, "invalid_argument",
                                              QStringLiteral("Expected a JSON object."));
                   }

                   const QString gender = body->value(QStringLiteral("gender")).toString();
                   const auto parsedGender = genderFromString(gender.toStdString());
                   if (!parsedGender) {
                       return Response::error(400, "invalid_argument",
                                              QStringLiteral("Unknown gender value."));
                   }

                   CreateAccountRequest req;
                   req.username = body->value(QStringLiteral("username")).toString().toStdString();
                   req.dateOfBirth = body->value(QStringLiteral("dob")).toString().toStdString();
                   req.gender = *parsedGender;

                   auto outcome = core.accounts().create(req);
                   if (outcome.isError()) return fromResult(outcome.result());

                   // Creating the account brings the radio up, so the Nearby
                   // screen is live the moment the UI navigates to it.
                   core.startAfterAccountCreated();

                   return Response::json(201, compact(json::accountToJson(outcome.value())));
               });

    router.add(Method::Patch, QStringLiteral("account"),
               [&core](const Request& request, const PathParams&) -> Response {
                   const auto body = parseBody(request);
                   if (!body) {
                       return Response::error(400, "invalid_argument",
                                              QStringLiteral("Expected a JSON object."));
                   }

                   UpdateAccountRequest update;
                   if (body->contains(QStringLiteral("username"))) {
                       update.username =
                           body->value(QStringLiteral("username")).toString().toStdString();
                   }
                   if (body->contains(QStringLiteral("gender"))) {
                       update.gender = genderFromString(
                           body->value(QStringLiteral("gender")).toString().toStdString());
                   }
                   if (body->contains(QStringLiteral("discoverable"))) {
                       update.discoverable = body->value(QStringLiteral("discoverable")).toBool();
                   }
                   if (body->contains(QStringLiteral("acceptFrom"))) {
                       update.acceptFrom = acceptFromString(
                           body->value(QStringLiteral("acceptFrom")).toString().toStdString());
                   }

                   // core.updateAccount, not accounts().update: a new name or
                   // discoverable flag has to reach the running mesh as well.
                   auto outcome = core.updateAccount(update);
                   if (outcome.isError()) return fromResult(outcome.result());
                   return Response::json(200, compact(json::accountToJson(outcome.value())));
               });

    // ── Discovery ───────────────────────────────────────────────────────────

    router.add(Method::Post, QStringLiteral("discovery"),
               [&core](const Request& request, const PathParams&) -> Response {
                   // {"mode":"active"|"passive"}. Active is the continuous scan the
                   // Nearby screen wants; passive is the background duty cycle that
                   // keeps the mesh reachable without flattening the battery. The
                   // body is optional, and omitting it means active — that is what
                   // every existing caller meant.
                   DiscoveryMode mode = DiscoveryMode::Active;
                   if (const auto body = parseBody(request); body) {
                       if (body->value(QStringLiteral("mode")).toString()
                           == QLatin1String("passive")) {
                           mode = DiscoveryMode::Passive;
                       }
                   }

                   const Result r = core.discovery().start(mode);
                   return r.isOk() ? Response::noContent() : fromResult(r);
               });

    router.add(Method::Delete, QStringLiteral("discovery"),
               [&core](const Request&, const PathParams&) -> Response {
                   core.discovery().stop();
                   return Response::noContent();
               });

    router.add(Method::Get, QStringLiteral("discovery/peers"),
               [&core](const Request&, const PathParams&) -> Response {
                   auto outcome = core.discovery().reachablePeers();
                   if (outcome.isError()) return fromResult(outcome.result());

                   // Each peer carries how the Nearby screen should present it,
                   // so the UI needs one fetch rather than a join against the
                   // request list — and so "Requested" survives navigating away.
                   QJsonArray array;
                   for (const Peer& peer : outcome.value()) {
                       const PeerPresentation view = core.requests().presentationFor(peer.peerId);
                       array.append(json::peerToJson(
                           peer, view.status, view.canResend,
                           QString::fromStdString(view.chatId.value())));
                   }
                   return Response::json(200, compact(array));
               });

    // ── Bluetooth ───────────────────────────────────────────────────────────
    //
    // v1 is Bluetooth-only, so the UI gates every in-app screen on this.
    //
    // It travels over the same loopback API as everything else rather than
    // through an injected JavaScript object. The page already authenticates by
    // knowing its secret URL (AD-03 / AD-08), so a bridge would add a second and
    // weaker path into the core — the reasoning is spelled out in
    // AnyTimeActivity.java, which installs no bridge for exactly this reason.
    //
    // Reading state is a query; enabling is a command. Neither reports the
    // outcome of the other: the radio actually coming up arrives as a
    // radio.state SSE event, from the transport, where that fact already lives.

    router.add(Method::Get, QStringLiteral("bluetooth"),
               [&core](const Request&, const PathParams&) -> Response {
                   IRadioController* radio = core.radio();

                   // A null controller is the DESKTOP case and must not read as
                   // broken: "unsupported" is what tells the UI there is nothing
                   // to gate on, which is what keeps browser development usable.
                   const RadioAvailability state =
                       radio ? radio->state() : RadioAvailability::Unsupported;

                   QJsonObject o;
                   o.insert(QStringLiteral("state"), QString::fromLatin1(toString(state)));
                   o.insert(QStringLiteral("canPromptInApp"),
                            radio != nullptr && radio->canPromptInApp());
                   return Response::json(200, compact(o));
               });

    router.add(Method::Post, QStringLiteral("bluetooth/enable"),
               [&core](const Request&, const PathParams&) -> Response {
                   IRadioController* radio = core.radio();
                   if (radio == nullptr) {
                       return Response::error(412, "precondition_failed",
                                              QStringLiteral("This platform has no radio."));
                   }
                   // 204, not 200-with-a-result: the user has not answered the
                   // system dialog yet, and inventing a boolean here would be a
                   // claim this code cannot support.
                   const Result r = radio->requestEnable();
                   return r.isOk() ? Response::noContent() : fromResult(r);
               });

    router.add(Method::Post, QStringLiteral("bluetooth/settings"),
               [&core](const Request&, const PathParams&) -> Response {
                   IRadioController* radio = core.radio();
                   if (radio == nullptr) {
                       return Response::error(412, "precondition_failed",
                                              QStringLiteral("This platform has no radio."));
                   }
                   const Result r = radio->openSettings();
                   return r.isOk() ? Response::noContent() : fromResult(r);
               });

    // ── Requests ────────────────────────────────────────────────────────────

    router.add(Method::Get, QStringLiteral("requests"),
               [&core](const Request&, const PathParams&) -> Response {
                   auto inbound = core.requests().inbound();
                   if (inbound.isError()) return fromResult(inbound.result());

                   QJsonArray array;
                   for (const anytime::Request& request : inbound.value()) {
                       array.append(json::requestToJson(request));
                   }
                   return Response::json(200, compact(array));
               });

    router.add(Method::Post, QStringLiteral("requests"),
               [&core](const Request& request, const PathParams&) -> Response {
                   const auto body = parseBody(request);
                   if (!body) {
                       return Response::error(400, "invalid_argument",
                                              QStringLiteral("Expected a JSON object."));
                   }

                   const auto peerId = PeerId::fromHex(
                       body->value(QStringLiteral("peerId")).toString().toStdString());
                   if (!peerId) {
                       return Response::error(400, "invalid_argument",
                                              QStringLiteral("Malformed peer id."));
                   }

                   auto sent = core.requests().send(
                       *peerId, body->value(QStringLiteral("greeting")).toString().toStdString());
                   if (sent.isError()) return fromResult(sent.result());
                   return Response::json(201, compact(json::requestToJson(sent.value())));
               });

    router.add(Method::Post, QStringLiteral("requests/:id/accept"),
               [&core](const Request&, const PathParams& params) -> Response {
                   auto chat = core.requests().accept(RequestId{params.value("id").toStdString()});
                   if (chat.isError()) return fromResult(chat.result());

                   // Return exactly what GET /chats/:id returns, so the chat
                   // screen the UI navigates to straight after accepting already
                   // knows the peer's name.
                   auto summary = core.chats().summaryFor(chat.value().id);
                   if (summary.isError()) return fromResult(summary.result());

                   ChatSummary row = summary.value();
                   row.reachable = reachablePeerIds(core).count(row.peerId) > 0;
                   return Response::json(200, compact(json::chatSummaryToJson(row)));
               });

    router.add(Method::Post, QStringLiteral("requests/:id/decline"),
               [&core](const Request& request, const PathParams& params) -> Response {
                   const auto body = parseBody(request);
                   const bool block =
                       body && body->value(QStringLiteral("block")).toBool(false);

                   const Result r = core.requests().decline(
                       RequestId{params.value("id").toStdString()}, block);
                   return r.isOk() ? Response::noContent() : fromResult(r);
               });

    // ── Chats ───────────────────────────────────────────────────────────────

    router.add(Method::Get, QStringLiteral("chats"),
               [&core](const Request&, const PathParams&) -> Response {
                   // summaries(), not list(): a bare Chat carries only a PeerId,
                   // so serialising one yields a row with an empty handle that
                   // the UI draws as a nameless "?" entry. The handle is built
                   // from the pinned contact rather than the live peer, so a chat
                   // still reads correctly when the other person is out of range.
                   auto chats = core.chats().summaries();
                   if (chats.isError()) return fromResult(chats.result());

                   const auto reachable = reachablePeerIds(core);

                   QJsonArray array;
                   for (ChatSummary summary : chats.value()) {
                       summary.reachable = reachable.count(summary.peerId) > 0;
                       array.append(json::chatSummaryToJson(summary));
                   }
                   return Response::json(200, compact(array));
               });

    router.add(Method::Get, QStringLiteral("chats/:id"),
               [&core](const Request&, const PathParams& params) -> Response {
                   auto summary =
                       core.chats().summaryFor(ChatId{params.value("id").toStdString()});
                   if (summary.isError()) return fromResult(summary.result());

                   ChatSummary row = summary.value();
                   row.reachable = reachablePeerIds(core).count(row.peerId) > 0;
                   return Response::json(200, compact(json::chatSummaryToJson(row)));
               });

    router.add(Method::Get, QStringLiteral("chats/:id/messages"),
               [&core](const Request& request, const PathParams& params) -> Response {
                   const QString before = request.queryValue("before");
                   auto page = core.chats().history(ChatId{params.value("id").toStdString()},
                                                    before.toStdString());
                   if (page.isError()) return fromResult(page.result());

                   QJsonArray messages;
                   for (const Message& message : page.value().messages) {
                       messages.append(json::messageToJson(message));
                   }

                   QJsonObject out;
                   out.insert(QStringLiteral("messages"), messages);
                   out.insert(QStringLiteral("nextCursor"),
                              page.value().nextCursor.empty()
                                  ? QJsonValue(QJsonValue::Null)
                                  : QJsonValue(QString::fromStdString(page.value().nextCursor)));
                   out.insert(QStringLiteral("hasMore"), page.value().hasMore);
                   return Response::json(200, compact(out));
               });

    router.add(Method::Post, QStringLiteral("chats/:id/messages"),
               [&core](const Request& request, const PathParams& params) -> Response {
                   const auto body = parseBody(request);
                   if (!body) {
                       return Response::error(400, "invalid_argument",
                                              QStringLiteral("Expected a JSON object."));
                   }

                   auto sent = core.chats().send(
                       ChatId{params.value("id").toStdString()},
                       body->value(QStringLiteral("body")).toString().toStdString(),
                       ClientMessageId{
                           body->value(QStringLiteral("clientMessageId")).toString().toStdString()});

                   if (sent.isError()) return fromResult(sent.result());
                   // 201 at status "queued" — the UI renders optimistically and
                   // every later transition arrives over SSE.
                   return Response::json(201, compact(json::messageToJson(sent.value())));
               });

    // The image itself is the raw request BODY, not JSON: base64 inside a JSON
    // document would cost a third more memory and parse time for no benefit on
    // a loopback socket. Metadata rides in the query string.
    //
    //   POST api/chats/<id>/media?mime=image/jpeg&clientMessageId=...&caption=...
    router.add(Method::Post, QStringLiteral("chats/:id/media"),
               [&core](const Request& request, const PathParams& params) -> Response {
                   if (request.body.isEmpty()) {
                       return Response::error(400, "invalid_argument",
                                              QStringLiteral("No image in the request body."));
                   }
                   // kMaxRequestBodyBytes already refused anything wildly large
                   // before this point; this is the protocol's own limit.
                   if (static_cast<std::size_t>(request.body.size())
                       > protocol::kMaxMediaBytes) {
                       return Response::error(
                           400, "invalid_argument",
                           QStringLiteral("That image is too large — resize it first."));
                   }

                   const QString clientMessageId = request.queryValue("clientMessageId");
                   if (clientMessageId.isEmpty()) {
                       return Response::error(400, "invalid_argument",
                                              QStringLiteral("clientMessageId is required."));
                   }

                   // Sniffed, not taken from the caller — see sniffImageType.
                   const QByteArray mime = sniffImageType(request.body);
                   if (mime == "application/octet-stream") {
                       return Response::error(400, "invalid_argument",
                                              QStringLiteral("That file is not a supported image."));
                   }

                   const auto* bytes = reinterpret_cast<const Byte*>(request.body.constData());
                   auto sent = core.chats().sendMedia(
                       ChatId{params.value("id").toStdString()},
                       mime.toStdString(),
                       request.queryValue("caption").toStdString(),
                       ByteSpan{bytes, static_cast<std::size_t>(request.body.size())},
                       ClientMessageId{clientMessageId.toStdString()});

                   if (sent.isError()) return fromResult(sent.result());
                   return Response::json(201, compact(json::messageToJson(sent.value())));
               });

    router.add(Method::Get, QStringLiteral("media/:id"),
               [&core](const Request&, const PathParams& params) -> Response {
                   auto bytes = core.chats().media(params.value("id").toStdString());
                   if (bytes.isError()) return fromResult(bytes.result());

                   Response response;
                   response.status = 200;
                   response.body = QByteArray(
                       reinterpret_cast<const char*>(bytes.value().data()),
                       static_cast<int>(bytes.value().size()));
                   response.contentType = sniffImageType(response.body);
                   return response;
               });

    router.add(Method::Post, QStringLiteral("chats/:id/read"),
               [&core](const Request& request, const PathParams& params) -> Response {
                   const auto body = parseBody(request);
                   if (!body) {
                       return Response::error(400, "invalid_argument",
                                              QStringLiteral("Expected a JSON object."));
                   }

                   const Result r = core.chats().markRead(
                       ChatId{params.value("id").toStdString()},
                       MessageId{
                           body->value(QStringLiteral("messageId")).toString().toStdString()});
                   return r.isOk() ? Response::noContent() : fromResult(r);
               });

    router.add(Method::Post, QStringLiteral("messages/:id/retry"),
               [&core](const Request&, const PathParams& params) -> Response {
                   // Through ChatService so a block also stops a resend.
                   const Result r =
                       core.chats().retry(MessageId{params.value("id").toStdString()});
                   return r.isOk() ? Response::noContent() : fromResult(r);
               });

    // ── Block list ──────────────────────────────────────────────────────────
    //
    // Keyed on the peer id — the key fingerprint — never a nickname, so a block
    // survives the peer renaming themselves (section 08).

    router.add(Method::Get, QStringLiteral("blocked"),
               [&core](const Request&, const PathParams&) -> Response {
                   auto blocked = core.blocks().list();
                   if (blocked.isError()) return fromResult(blocked.result());

                   QJsonArray array;
                   for (const BlockedPeer& row : blocked.value()) {
                       array.append(json::blockedPeerToJson(row));
                   }
                   return Response::json(200, compact(array));
               });

    router.add(Method::Post, QStringLiteral("blocked"),
               [&core](const Request& request, const PathParams&) -> Response {
                   const auto body = parseBody(request);
                   if (!body) {
                       return Response::error(400, "invalid_argument",
                                              QStringLiteral("Expected a JSON object."));
                   }
                   const auto peerId = PeerId::fromHex(
                       body->value(QStringLiteral("peerId")).toString().toStdString());
                   if (!peerId) {
                       return Response::error(400, "invalid_argument",
                                              QStringLiteral("Malformed peer id."));
                   }
                   const Result r = core.blocks().block(*peerId);
                   return r.isOk() ? Response::noContent() : fromResult(r);
               });

    router.add(Method::Delete, QStringLiteral("blocked/:peerId"),
               [&core](const Request&, const PathParams& params) -> Response {
                   const auto peerId = PeerId::fromHex(params.value("peerId").toStdString());
                   if (!peerId) {
                       return Response::error(400, "invalid_argument",
                                              QStringLiteral("Malformed peer id."));
                   }
                   const Result r = core.blocks().unblock(*peerId);
                   return r.isOk() ? Response::noContent() : fromResult(r);
               });
}

}  // namespace anytime::http
