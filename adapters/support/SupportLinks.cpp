// adapters/support/SupportLinks.cpp

#include "adapters/support/SupportLinks.h"

#include <QByteArray>
#include <QStringList>

namespace anytime::support {

namespace {

constexpr const char* kPrivacyPolicyUrl = "https://techyskitter.com/anytime/privacy-policy";
constexpr const char* kChildSafetyUrl   = "https://techyskitter.com/anytime/child-safety";

}  // namespace

std::optional<ReportReason> reportReasonFromString(const QString& id)
{
    if (id == QLatin1String("child_safety"))   return ReportReason::ChildSafety;
    if (id == QLatin1String("sexual_content")) return ReportReason::SexualContent;
    if (id == QLatin1String("harassment"))     return ReportReason::Harassment;
    if (id == QLatin1String("spam"))           return ReportReason::Spam;
    if (id == QLatin1String("other"))          return ReportReason::Other;
    return std::nullopt;
}

QString describe(ReportReason reason)
{
    switch (reason) {
        case ReportReason::ChildSafety:   return QStringLiteral("Child safety concern");
        case ReportReason::SexualContent: return QStringLiteral("Nudity or sexual content");
        case ReportReason::Harassment:    return QStringLiteral("Harassment or threats");
        case ReportReason::Spam:          return QStringLiteral("Spam or scam");
        case ReportReason::Other:         return QStringLiteral("Something else");
    }
    return QStringLiteral("Something else");
}

QUrl reportMailto(const ReportDetails& d)
{
    const QString subject = QStringLiteral("AnyTime report: ") + describe(d.reason);

    // CRLF, which is what RFC 6068 asks of a mailto body. The blank space at
    // the end is where the user writes; everything above it is filled in so
    // they do not have to find a key fingerprint themselves.
    const QStringList lines{
        QStringLiteral("Reason: ") + describe(d.reason),
        QStringLiteral("Reported user: ") + d.reportedHandle + QStringLiteral(" (ID ")
            + d.reportedPeerId + QStringLiteral(")"),
        QStringLiteral("Reported by: ") + d.reporterHandle + QStringLiteral(" (ID ")
            + d.reporterPeerId + QStringLiteral(")"),
        QStringLiteral("App version: ") + d.appVersion,
        QString(),
        QStringLiteral("Please describe what happened. AnyTime chats are end-to-end encrypted "
                       "and never reach us, so we can only see what you write here:"),
        QString(),
        QString(),
    };
    const QString body = lines.join(QStringLiteral("\r\n"));

    // Built by hand from fully percent-encoded pieces rather than through
    // QUrlQuery, which leaves '&' and '=' alone inside a value — exactly the
    // characters that must not survive from an untrusted nickname.
    QByteArray encoded("mailto:");
    encoded += kSupportEmail;
    encoded += "?subject=" + QUrl::toPercentEncoding(subject);
    encoded += "&body=" + QUrl::toPercentEncoding(body);
    return QUrl::fromEncoded(encoded, QUrl::StrictMode);
}

std::optional<QUrl> externalLink(const QString& id)
{
    if (id == QLatin1String("privacy"))      return QUrl(QString::fromLatin1(kPrivacyPolicyUrl));
    if (id == QLatin1String("child-safety")) return QUrl(QString::fromLatin1(kChildSafetyUrl));
    return std::nullopt;
}

}  // namespace anytime::support
