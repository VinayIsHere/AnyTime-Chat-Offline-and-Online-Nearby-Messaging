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
/// A nearby peer, plus how the screen should present it. `status` and
/// `canResend` are supplied by the caller because they come from the request
/// gate, which this mapping layer has no business reaching into.
QJsonObject peerToJson(const Peer&, PeerStatus, bool canResend, const QString& chatId);
QJsonObject requestToJson(const Request&);
/// Serialises a bare Chat. The caller MUST supply the handle — a Chat carries
/// only a PeerId, so there is nothing here to build a name from. Prefer
/// chatSummaryToJson, which cannot be called without one.
QJsonObject chatToJson(const Chat&, const QString& handle);

/// The chat-list row: handle, preview and reachability all filled in.
QJsonObject chatSummaryToJson(const ChatSummary&);
QJsonObject messageToJson(const Message&);

/// One block-list row. The handle falls back to the bare fingerprint suffix
/// when no name was ever learned, which is still enough to tell rows apart.
QJsonObject blockedPeerToJson(const BlockedPeer&);

/// Relative time as the UI renders it directly: "now", "4m", "2h", "3d".
QString relativeTime(Timestamp then, Timestamp now);

}  // namespace anytime::http::json
