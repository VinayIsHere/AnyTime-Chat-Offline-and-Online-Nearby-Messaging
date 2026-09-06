// adapters/http/Router.cpp

#include "adapters/http/Router.h"

namespace anytime::http {

QStringList Router::splitPath(const QString& path)
{
    return path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
}

void Router::add(Method method, const QString& pattern, Handler handler)
{
    routes_.push_back(Route{method, splitPath(pattern), std::move(handler)});
}

bool Router::match(const QStringList& pattern, const QStringList& path, PathParams& out)
{
    if (pattern.size() != path.size()) return false;

    for (int i = 0; i < pattern.size(); ++i) {
        const QString& p = pattern.at(i);
        if (p.startsWith(QLatin1Char(':'))) {
            const QString value = path.at(i);
            if (value.isEmpty()) return false;
            out.insert(p.mid(1), value);
        } else if (p != path.at(i)) {
            return false;
        }
    }
    return true;
}

std::optional<Response> Router::dispatch(const Request& request) const
{
    const QStringList path = splitPath(request.path);

    for (const Route& route : routes_) {
        if (route.method != request.method) continue;
        PathParams params;
        if (match(route.segments, path, params)) return route.handler(request, params);
    }
    return std::nullopt;
}

bool Router::pathExistsForOtherMethod(const Request& request) const
{
    const QStringList path = splitPath(request.path);

    for (const Route& route : routes_) {
        if (route.method == request.method) continue;
        PathParams ignored;
        if (match(route.segments, path, ignored)) return true;
    }
    return false;
}

}  // namespace anytime::http
