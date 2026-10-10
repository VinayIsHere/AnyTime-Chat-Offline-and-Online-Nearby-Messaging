// adapters/notify/AndroidNotificationPresenter.h
//
// INotificationPresenter over platform/android/src/com/anytimechat/MessageNotifier.java.
//
// A JNI seam with no policy of its own, like AndroidRadioController. Whether to
// show anything at all — the app is in the foreground, or the user declined the
// notification permission — is decided on the Java side, where that state is.
//
// Compiled only on Android — see the if(ANDROID) block in CMakeLists.txt.

#pragma once

#include "adapters/notify/Notifier.h"

namespace anytime::notify {

class AndroidNotificationPresenter final : public INotificationPresenter {
public:
    void show(const Notice&) override;
};

}  // namespace anytime::notify
