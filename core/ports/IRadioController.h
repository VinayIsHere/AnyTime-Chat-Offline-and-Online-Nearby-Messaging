// core/ports/IRadioController.h
//
// Turning the radio ON is a different concern from carrying bytes over it.
//
// ITransport answers "can I send?" and reports availability as it changes, but
// it deliberately cannot ASK the platform to become available: that is an
// interactive, Activity-scoped operation that means nothing on a loopback
// socket, and putting it on ITransport would force every transport to pretend
// it had an opinion about a system dialog.
//
// This is that second concern, and it is all the Bluetooth gate screen needs.
//
//   Android — ACTION_REQUEST_ENABLE, so canPromptInApp() is true.
//   iOS     — CoreBluetooth has no in-app switch; canPromptInApp() is false and
//             openSettings() is the only route. Not implemented in v1.
//   Desktop — supplies no implementation at all. AppCore then reports
//             Unsupported, which the UI treats as "there is nothing to gate on"
//             rather than as a fault, so browser development stays usable.

#pragma once

#include "core/domain/Enums.h"
#include "core/domain/Result.h"

namespace anytime {

class IRadioController {
public:
    virtual ~IRadioController() = default;

    /// What the platform says right now. Cheap, and deliberately not cached —
    /// see the note on the Android adapter.
    virtual RadioAvailability state() const = 0;

    /// True where the OS offers an in-app switch, false where the system
    /// settings app is the only way. The UI changes its wording on this rather
    /// than offering a button that cannot work.
    virtual bool canPromptInApp() const = 0;

    /// Asks the platform to make the radio usable: the permission prompt when
    /// permissions are what is missing, otherwise the system enable dialog.
    ///
    /// RETURNS IMMEDIATELY, and the answer never comes back through this call.
    /// The user may take seconds, switch away, or never decide, so there is no
    /// honest value to return here. The outcome arrives as an ordinary
    /// radio.state event once a transport comes back up — the same path taken
    /// when someone flips Bluetooth from the notification shade instead.
    virtual Result requestEnable() = 0;

    /// Opens the system Bluetooth settings screen.
    virtual Result openSettings() = 0;
};

}  // namespace anytime
