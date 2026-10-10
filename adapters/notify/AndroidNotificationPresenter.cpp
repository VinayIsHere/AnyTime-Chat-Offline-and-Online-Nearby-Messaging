// adapters/notify/AndroidNotificationPresenter.cpp

#include "adapters/notify/AndroidNotificationPresenter.h"

#include <QCoreApplication>
#include <QJniObject>
#include <QtCore/qcoreapplication_platform.h>

#include "core/log/Log.h"

namespace anytime::notify {

namespace {

constexpr const char* kNotifierClass = "com/anytimechat/MessageNotifier";

/// Mirrors MessageNotifier.KIND_*. Kept explicit rather than cast so a new kind
/// on either side fails loudly here.
jint kindForJava(NoticeKind kind)
{
    switch (kind) {
        case NoticeKind::Message: return 0;
        case NoticeKind::Request: return 1;
    }
    return 0;
}

QJniObject javaString(const std::string& s)
{
    return QJniObject::fromString(QString::fromStdString(s));
}

}  // namespace

void AndroidNotificationPresenter::show(const Notice& notice)
{
    const QJniObject context = QNativeInterface::QAndroidApplication::context();
    if (!context.isValid()) return;

    QJniObject::callStaticMethod<void>(
        kNotifierClass, "show",
        "(Landroid/content/Context;ILjava/lang/String;Ljava/lang/String;Ljava/lang/String;"
        "Ljava/lang/String;)V",
        context.object(), kindForJava(notice.kind), javaString(notice.thread).object<jstring>(),
        javaString(notice.title).object<jstring>(), javaString(notice.text).object<jstring>(),
        javaString(notice.route).object<jstring>());

    // The thread id only — the title and text carry a name and a message body.
    AT_LOG_DEBUG("notify", "notification handed to Android for {}", notice.thread);
}

}  // namespace anytime::notify
