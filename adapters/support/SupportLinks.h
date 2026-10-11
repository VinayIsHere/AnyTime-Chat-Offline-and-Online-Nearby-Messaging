// adapters/support/SupportLinks.h
//
// The app's two ways out to us: a report about another user, sent by email, and
// the handful of pages on our website the app links to.
//
// WHY A REPORT IS AN EMAIL. AnyTime has no server, and conversations are
// end-to-end encrypted, so there is nowhere for a report to be posted to and
// nothing we could look up if there were. What a report CAN carry is who is
// reporting whom, and why — and the user's own account of what happened, which
// only they can write. So reporting blocks the person on this device and opens
// the phone's mail app with a draft addressed to us. The user reads it and
// presses send; nothing leaves the device without that.
//
// WHY THIS LIVES HERE AND NOT IN THE PAGE. The page could build a mailto: link
// itself, but a WebView that cannot hand the link to a mail app navigates to an
// error page instead — which would replace the whole UI. The core opens it
// through the platform and reports back whether anything answered, and the page
// then shows the address to write to. It also means every address and URL the
// app can open is a constant in this file: the page names a link by id, it never
// supplies a URL.

#pragma once

#include <optional>

#include <QString>
#include <QUrl>

namespace anytime::support {

/// Where reports and questions go. The same address the privacy policy gives.
inline constexpr const char* kSupportEmail = "contact@techyskitter.com";

/// The reasons the report sheet offers. Persisted nowhere; the string ids are
/// only the API's vocabulary and mirror ReportReason in ui/src/api/client.ts.
enum class ReportReason { ChildSafety, SexualContent, Harassment, Spam, Other };

/// "child_safety", "sexual_content", "harassment", "spam", "other". Anything
/// else is refused, so a caller cannot put arbitrary text into the subject.
std::optional<ReportReason> reportReasonFromString(const QString& id);

/// The wording used in the email, for a person to read.
QString describe(ReportReason);

struct ReportDetails {
    ReportReason reason = ReportReason::Other;
    QString reportedHandle;   ///< "@name·abcd" — the name is chosen by THEM, so it is untrusted
    QString reportedPeerId;   ///< hex key fingerprint; what actually identifies them
    QString reporterHandle;
    QString reporterPeerId;
    QString appVersion;
};

/// A mailto: URL with the subject and body filled in.
///
/// Every value is percent-encoded in full. The reported handle comes from
/// another device and is whatever its owner typed: left raw, a name containing
/// "&cc=" or a line break could add recipients or headers to the draft.
QUrl reportMailto(const ReportDetails&);

/// The pages the app may open in the browser, by id: "privacy", "child-safety".
/// Unknown ids return nothing — the page cannot make the app open a URL of its
/// choosing.
std::optional<QUrl> externalLink(const QString& id);

}  // namespace anytime::support
