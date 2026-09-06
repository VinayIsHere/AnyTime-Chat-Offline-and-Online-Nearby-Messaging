// adapters/http/Router.h
//
// Pattern-matched dispatch for the local API.
//
// Patterns use ":name" for a single path segment, e.g. "chats/:id/messages".
// Matching is exact on segment count, so "chats/a/b" never reaches a handler
// registered for "chats/:id".

#pragma once

#include <QHash>
#include <QString>
#include <QStringList>

#include <functional>
#include <vector>

#include "adapters/http/HttpTypes.h"

namespace anytime::http {

/// Captured ":name" segments from the matched pattern.
using PathParams = QHash<QString, QString>;

using Handler = std::function<Response(const Request&, const PathParams&)>;

class Router {
public:
    /// `pattern` is relative to the API root, with no leading slash:
    /// "account", "requests/:id/accept", "chats/:id/messages".
    void add(Method, const QString& pattern, Handler);

    /// Returns nullopt when nothing matched, so the caller can 404.
    std::optional<Response> dispatch(const Request&) const;

    /// True when the path matches a route registered under a DIFFERENT method —
    /// lets the caller answer 405 rather than a misleading 404.
    bool pathExistsForOtherMethod(const Request&) const;

private:
    struct Route {
        Method      method;
        QStringList segments;
        Handler     handler;
    };

    static QStringList splitPath(const QString&);
    static bool        match(const QStringList& pattern, const QStringList& path, PathParams& out);

    std::vector<Route> routes_;
};

}  // namespace anytime::http
