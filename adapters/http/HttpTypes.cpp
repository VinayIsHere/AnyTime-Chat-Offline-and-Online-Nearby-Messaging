// adapters/http/HttpTypes.cpp

#include "adapters/http/HttpTypes.h"

#include <QUrl>

#include "core/domain/Types.h"

namespace anytime::http {

Method methodFromToken(const QByteArray& token) noexcept
{
    if (token == "GET")     return Method::Get;
    if (token == "POST")    return Method::Post;
    if (token == "PATCH")   return Method::Patch;
    if (token == "DELETE")  return Method::Delete;
    if (token == "OPTIONS") return Method::Options;
    return Method::Unsupported;
}

QString methodName(Method m) noexcept
{
    switch (m) {
        case Method::Get:     return QStringLiteral("GET");
        case Method::Post:    return QStringLiteral("POST");
        case Method::Patch:   return QStringLiteral("PATCH");
        case Method::Delete:  return QStringLiteral("DELETE");
        case Method::Options: return QStringLiteral("OPTIONS");
        case Method::Unsupported: break;
    }
    return QStringLiteral("?");
}

// ─────────────────────────────────────────────────────────────────────────────

Response Response::json(int status, const QByteArray& body)
{
    Response r;
    r.status = status;
    r.contentType = "application/json; charset=utf-8";
    r.body = body;
    return r;
}

Response Response::noContent()
{
    Response r;
    r.status = 204;
    r.contentType.clear();
    return r;
}

Response Response::error(int status, const char* code, const QString& message)
{
    QByteArray escaped = message.toUtf8().replace('\\', "\\\\").replace('"', "\\\"");
    return json(status,
                "{\"code\":\"" + QByteArray(code) + "\",\"message\":\"" + escaped + "\"}");
}

QByteArray Response::serialise() const
{
    static const auto reason = [](int s) -> const char* {
        switch (s) {
            case 200: return "OK";
            case 201: return "Created";
            case 204: return "No Content";
            case 400: return "Bad Request";
            case 403: return "Forbidden";
            case 404: return "Not Found";
            case 405: return "Method Not Allowed";
            case 413: return "Payload Too Large";
            case 500: return "Internal Server Error";
            default:  return "OK";
        }
    };

    QByteArray out;
    out += "HTTP/1.1 " + QByteArray::number(status) + ' ' + reason(status) + "\r\n";
    if (!contentType.isEmpty()) out += "Content-Type: " + contentType + "\r\n";

    // No CORS headers, ever (docs/ARCHITECTURE.md section 10). Their absence is
    // what stops a hostile page reading responses even if it guesses the URL.
    out += "Cache-Control: no-store\r\n";
    out += "X-Content-Type-Options: nosniff\r\n";

    if (keepOpen) {
        out += "Connection: keep-alive\r\n\r\n";
    } else {
        out += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
        out += "Connection: close\r\n\r\n";
        out += body;
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────

ParseState parseRequest(const QByteArray& buffer, Request& out, int& consumedBytes)
{
    const int headerEnd = buffer.indexOf("\r\n\r\n");
    if (headerEnd < 0) {
        // Guard against a peer that opens a socket and never finishes a request.
        return buffer.size() > static_cast<int>(kMaxRequestBodyBytes) ? ParseState::Malformed
                                                                     : ParseState::NeedMoreData;
    }

    const QByteArray head = buffer.left(headerEnd);
    const QList<QByteArray> lines = head.split('\n');
    if (lines.isEmpty()) return ParseState::Malformed;

    // ── Request line: METHOD target HTTP/1.1 ────────────────────────────────
    const QList<QByteArray> parts = lines.first().trimmed().split(' ');
    if (parts.size() < 2) return ParseState::Malformed;

    out.method = methodFromToken(parts.at(0));
    out.rawTarget = QString::fromUtf8(parts.at(1));

    QString target = out.rawTarget;
    const int q = target.indexOf(QLatin1Char('?'));
    if (q >= 0) {
        const QString qs = target.mid(q + 1);
        target = target.left(q);
        for (const QString& pair : qs.split(QLatin1Char('&'), Qt::SkipEmptyParts)) {
            const int eq = pair.indexOf(QLatin1Char('='));
            if (eq < 0) out.query.insert(QUrl::fromPercentEncoding(pair.toUtf8()), QString());
            else
                out.query.insert(QUrl::fromPercentEncoding(pair.left(eq).toUtf8()),
                                 QUrl::fromPercentEncoding(pair.mid(eq + 1).toUtf8()));
        }
    }
    out.path = QUrl::fromPercentEncoding(target.toUtf8());

    // ── Headers ─────────────────────────────────────────────────────────────
    for (int i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines.at(i).trimmed();
        if (line.isEmpty()) continue;
        const int colon = line.indexOf(':');
        if (colon <= 0) continue;
        out.headers.insert(QString::fromLatin1(line.left(colon)).trimmed().toLower(),
                           line.mid(colon + 1).trimmed());
    }

    // ── Body ────────────────────────────────────────────────────────────────
    const int bodyStart = headerEnd + 4;
    bool ok = false;
    const int declared = out.header("content-length").toInt(&ok);
    const int length = ok ? declared : 0;

    if (length < 0 || length > static_cast<int>(kMaxRequestBodyBytes)) return ParseState::Malformed;
    if (buffer.size() - bodyStart < length) return ParseState::NeedMoreData;

    out.body = buffer.mid(bodyStart, length);
    consumedBytes = bodyStart + length;
    return ParseState::Complete;
}

}  // namespace anytime::http
