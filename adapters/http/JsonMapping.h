// adapters/http/JsonMapping.h
//
// The single place domain entities become JSON. Field names here ARE the local
// API contract — they must match the interfaces in ui/src/api/client.ts, and
// changing one without the other is a silent breakage.

#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include "core/domain/Entities.h"

namespace anytime::http::json {

/// "@vinay·4f2a". Built here rather than in the UI so C++ owns the one
/// definition of a handle (AD-05).
QString handleFor(const std::string& nickname, const PeerId&);

QJsonObject accountToJson(const Account&);
QJsonObject peerToJson(const Peer&);
QJsonObject requestToJson(const Request&);
QJsonObject chatToJson(const Chat&, const QString& handle);
QJsonObject messageToJson(const Message&);

/// Relative time as the UI renders it directly: "now", "4m", "2h", "3d".
QString relativeTime(Timestamp then, Timestamp now);

}  // namespace anytime::http::json
