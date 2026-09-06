// core/ports/IClock.h
//
// Injectable time. Every timestamp, timeout and retry schedule in the core comes
// from here — nothing calls std::chrono::system_clock directly.
//
// The point is testability: the outbox retry ladder, the 60 s peer reachability
// window and the announce backoff are all time-driven, and tests must exercise
// them without sleeping. A FakeClock in tests/ advances on demand.

#pragma once

#include <cstdint>
#include <functional>

#include "core/domain/Types.h"

namespace anytime {

// TimerId and kInvalidTimer live in core/domain/Identifiers.h.

class IClock {
public:
    virtual ~IClock() = default;

    /// Milliseconds since the Unix epoch.
    virtual Timestamp now() const = 0;

    /// Monotonic milliseconds since an arbitrary origin. Use for durations —
    /// unlike now(), this does not jump when the system clock is corrected.
    virtual Timestamp monotonic() const = 0;

    /// Runs `fn` on the core thread after `delayMillis`. Single-shot.
    virtual TimerId scheduleOnce(Timestamp delayMillis, std::function<void()> fn) = 0;

    /// Runs `fn` on the core thread every `intervalMillis` until cancelled.
    virtual TimerId scheduleRepeating(Timestamp intervalMillis, std::function<void()> fn) = 0;

    virtual void cancel(TimerId) = 0;
};

}  // namespace anytime
