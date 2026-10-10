// adapters/radio/AndroidRadioController.cpp

#include "adapters/radio/AndroidRadioController.h"

#include <QCoreApplication>
#include <QJniObject>
#include <QtCore/qcoreapplication_platform.h>

#include "core/log/Log.h"

namespace anytime::radio {

namespace {

constexpr const char* kBridgeClass = "com/anytimechat/BluetoothBridge";

/// Mirrors the constants in BluetoothBridge.java. Kept as a switch rather than
/// a cast so adding a state on either side fails loudly here instead of
/// silently becoming a number the UI has never heard of.
RadioAvailability fromJava(int value)
{
    switch (value) {
        case 1: return RadioAvailability::Unsupported;
        case 2: return RadioAvailability::Unauthorized;
        case 3: return RadioAvailability::Off;
        case 4: return RadioAvailability::On;
        default: return RadioAvailability::Unknown;
    }
}

}  // namespace

RadioAvailability AndroidRadioController::state() const
{
    const QJniObject context = QNativeInterface::QAndroidApplication::context();
    if (!context.isValid()) return RadioAvailability::Unknown;

    const jint raw = QJniObject::callStaticMethod<jint>(
        kBridgeClass, "state", "(Landroid/content/Context;)I", context.object());
    return fromJava(static_cast<int>(raw));
}

bool AndroidRadioController::canPromptInApp() const
{
    // Constant true on Android, but asked of Java rather than hard-coded here:
    // the answer belongs with the code that raises the dialog, so a future
    // build that cannot show one has a single place to say so.
    return QJniObject::callStaticMethod<jboolean>(kBridgeClass, "canPromptInApp", "()Z");
}

Result AndroidRadioController::requestEnable()
{
    const QJniObject context = QNativeInterface::QAndroidApplication::context();
    if (!context.isValid()) {
        return Result::error(ErrorCode::Internal, "No Android context.");
    }

    QJniObject::callStaticMethod<void>(kBridgeClass, "requestEnable",
                                       "(Landroid/content/Context;)V", context.object());

    // Ok means "the prompt was asked for", never "the radio is on". The user
    // may take seconds or never answer; the real outcome arrives as a
    // radio.state event when a transport comes back up.
    AT_LOG_INFO("radio", "asked Android to enable the radio");
    return Result::ok();
}

Result AndroidRadioController::openSettings()
{
    const QJniObject context = QNativeInterface::QAndroidApplication::context();
    if (!context.isValid()) {
        return Result::error(ErrorCode::Internal, "No Android context.");
    }

    QJniObject::callStaticMethod<void>(kBridgeClass, "openSettings",
                                       "(Landroid/content/Context;)V", context.object());
    return Result::ok();
}

}  // namespace anytime::radio
