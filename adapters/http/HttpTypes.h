// adapters/http/HttpTypes.h
//
// Minimal HTTP/1.1 request and response shapes for the loopback API server.
//
// Deliberately small: this server answers one client (our own WebView) on
// 127.0.0.1 and must never be exposed off-device. No chunked encoding, no
// keep-alive except for SSE, no compression. Anything more would be surface
// area we do not need and would have to secure.

#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>

namespace anytime::http {

enum class Method { Get, Post, Patch, Delete, Options, Unsupported };

Method  methodFromToken(const QByteArray&) noexcept;
QString methodName(Method) noexcept;

struct Request {
    Method  method = Method::Unsupported;
    QString path;          ///< decoded, with the secret prefix already stripped
    QString rawTarget;     ///< original request target, for logging
    QHash<QString, QString> query;
    QHash<QString, QByteArray> headers;  ///< keys lower-cased
    QByteArray body;

    QByteArray header(const char* name) const { return headers.value(QString::fromLatin1(name)); }
    QString    queryValue(const char* key) const { return query.value(QString::fromLatin1(key)); }
};

struct Response {
    int        status = 200;
    QByteArray contentType = "application/json";
    QByteArray body;

    /// Set for the SSE stream, which must not be closed after writing.
    bool keepOpen = false;

    static Response json(int status, const QByteArray& body);
    static Response noContent();

    /// Error body shape the React client parses: { "code": ..., "message": ... }
    /// `code` matches anytime::ErrorCode's string form, so the UI can branch on
    /// it — ApiError.code in ui/src/api/client.ts.
    static Response error(int status, const char* code, const QString& message);

    QByteArray serialise() const;
};

/// Parse result for one request off the socket.
enum class ParseState { NeedMoreData, Complete, Malformed };

/// Parses request line, headers and body. Returns NeedMoreData until the whole
/// body has arrived, so callers can keep accumulating.
ParseState parseRequest(const QByteArray& buffer, Request& out, int& consumedBytes);

}  // namespace anytime::http
