// adapters/radio/AndroidRadioController.h
//
// IRadioController over platform/android/src/com/anytimechat/BluetoothBridge.java.
//
// Java rather than Qt for the same reason PermissionBridge is Java: the Android
// 12 Bluetooth split and ACTION_REQUEST_ENABLE have no Qt wrapper, and Qt
// Bluetooth is deliberately not linked (AD-02). This class is the thin JNI seam
// and holds no policy of its own.
//
// Compiled only on Android — see the if(ANDROID) block in CMakeLists.txt.

#pragma once

#include "core/ports/IRadioController.h"

namespace anytime::radio {

/// STATELESS BY DESIGN. Every call asks Android fresh.
///
/// Caching adapter state here would create a second copy of the truth that goes
/// stale exactly when it matters most: while the user is inside the system
/// dialog, which is the one moment this class cannot observe. Each query is a
/// couple of JNI calls and happens on user action or on an SSE event, never in
/// a loop, so there is nothing to optimise away.
class AndroidRadioController final : public IRadioController {
public:
    RadioAvailability state() const override;
    bool              canPromptInApp() const override;
    Result            requestEnable() override;
    Result            openSettings() override;
};

}  // namespace anytime::radio
