// tests/SupportLinksTests.cpp
//
// The report email is assembled from a name another device chose. These tests
// are mostly about that: whatever the name contains, the draft still goes to
// us, and only to us.

#include "tests/TestHarness.h"

#include "adapters/support/SupportLinks.h"

#include <QUrlQuery>

namespace anytime::test {

namespace {

support::ReportDetails sampleReport()
{
    support::ReportDetails d;
    d.reason         = support::ReportReason::Harassment;
    d.reportedHandle = QStringLiteral("@riya·9fc0");
    d.reportedPeerId = QStringLiteral("fe2864d89fff9fc0");
    d.reporterHandle = QStringLiteral("@vinay·48b8");
    d.reporterPeerId = QStringLiteral("1f3f89e96dd948b8");
    d.appVersion     = QStringLiteral("0.1");
    return d;
}

}  // namespace

void runSupportLinksTests(Registry& registry)
{
    registry.section("Report email");
    {
        const QUrl url = support::reportMailto(sampleReport());
        const QUrlQuery query(url);

        registry.check(url.isValid() && url.scheme() == QLatin1String("mailto"),
                       "is a valid mailto link");
        registry.check(url.path() == QLatin1String(support::kSupportEmail),
                       "is addressed to the support address");
        registry.check(query.queryItemValue(QStringLiteral("subject"), QUrl::FullyDecoded)
                           == QLatin1String("AnyTime report: Harassment or threats"),
                       "says what it is about in the subject");

        const QString body = query.queryItemValue(QStringLiteral("body"), QUrl::FullyDecoded);
        registry.check(body.contains(QStringLiteral("Reported user: @riya·9fc0 (ID fe2864d89fff9fc0)")),
                       "names the reported user and their id");
        registry.check(body.contains(QStringLiteral("Reported by: @vinay·48b8 (ID 1f3f89e96dd948b8)")),
                       "names who is reporting");
        registry.check(body.contains(QStringLiteral("\r\n")) && !body.contains(QStringLiteral("\n\n")),
                       "uses CRLF line breaks throughout");
    }

    registry.section("Report email — a hostile name cannot redirect it");
    {
        // What a peer could set as their nickname to try to copy the report to
        // themselves, or to smuggle a header into the draft.
        support::ReportDetails d = sampleReport();
        d.reportedHandle =
            QStringLiteral("@x&cc=attacker@example.com&bcc=a@b.c\r\nBcc: attacker@example.com#?=%");

        const QUrl url = support::reportMailto(d);
        const QUrlQuery query(url);

        registry.check(url.isValid(), "is still a valid link");
        registry.check(url.path() == QLatin1String(support::kSupportEmail),
                       "still goes to the support address alone");
        registry.check(query.queryItems().size() == 2,
                       "has exactly a subject and a body, nothing added");
        registry.check(!query.hasQueryItem(QStringLiteral("cc"))
                           && !query.hasQueryItem(QStringLiteral("bcc")),
                       "gained no cc or bcc");
        registry.check(query.queryItemValue(QStringLiteral("body"), QUrl::FullyDecoded)
                           .contains(d.reportedHandle),
                       "and carries the name through verbatim, as text");
    }

    registry.section("Report reasons and links are a closed list");
    {
        registry.check(support::reportReasonFromString(QStringLiteral("child_safety"))
                           == support::ReportReason::ChildSafety,
                       "a known reason is accepted");
        registry.check(!support::reportReasonFromString(QStringLiteral("Free text here")).has_value()
                           && !support::reportReasonFromString(QString()).has_value(),
                       "anything else is refused");

        const auto privacy = support::externalLink(QStringLiteral("privacy"));
        const auto safety  = support::externalLink(QStringLiteral("child-safety"));
        registry.check(privacy && privacy->scheme() == QLatin1String("https")
                           && privacy->host() == QLatin1String("techyskitter.com"),
                       "the privacy link is our own https page");
        registry.check(safety && safety->host() == QLatin1String("techyskitter.com"),
                       "the child-safety link is our own page");
        registry.check(!support::externalLink(QStringLiteral("https://evil.example")).has_value(),
                       "a URL passed as an id opens nothing");
    }
}

}  // namespace anytime::test
