// adapters/http/ApiRoutes.cpp

#include "adapters/http/ApiRoutes.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "adapters/http/JsonMapping.h"
#include "adapters/http/Router.h"
#include "adapters/http/SseHub.h"
#include "core/AppCore.h"
#include "core/services/AccountService.h"
#include "core/services/DiscoveryService.h"

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

                   auto outcome = core.accounts().update(update);
                   if (outcome.isError()) return fromResult(outcome.result());
                   return Response::json(200, compact(json::accountToJson(outcome.value())));
               });

    // ── Discovery ───────────────────────────────────────────────────────────

    router.add(Method::Post, QStringLiteral("discovery"),
               [&core](const Request&, const PathParams&) -> Response {
                   const Result r = core.discovery().start(DiscoveryMode::Active);
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

                   QJsonArray array;
                   for (const Peer& peer : outcome.value()) array.append(json::peerToJson(peer));
                   return Response::json(200, compact(array));
               });

    // ── Requests and chats ──────────────────────────────────────────────────
    //
    // Not implemented yet — these land with the messaging milestone. They are
    // registered so the UI receives a well-formed empty list rather than a 404
    // it would surface as a hard error.

    router.add(Method::Get, QStringLiteral("requests"),
               [](const Request&, const PathParams&) -> Response {
                   return Response::json(200, "[]");
               });

    router.add(Method::Get, QStringLiteral("chats"),
               [](const Request&, const PathParams&) -> Response {
                   return Response::json(200, "[]");
               });

    router.add(Method::Get, QStringLiteral("chats/:id/messages"),
               [](const Request&, const PathParams&) -> Response {
                   return Response::json(
                       200, R"({"messages":[],"nextCursor":null,"hasMore":false})");
               });
}

}  // namespace anytime::http
