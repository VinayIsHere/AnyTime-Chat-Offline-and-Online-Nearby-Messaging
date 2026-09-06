// adapters/clock/QtClock.h
//
// IClock over QDateTime and QTimer. Every timer fires on the core thread,
// because QTimer delivers on the thread that owns it and the core objects all
// live there.
//
// tests/ supplies a FakeClock instead, so the outbox retry ladder, the 60 s
// reachability window and the announce backoff can all be exercised without
// sleeping.

#pragma once

#include <QObject>

#include <memory>

#include "core/ports/IClock.h"

namespace anytime::platform {

class QtClock final : public QObject, public IClock {
    Q_OBJECT

public:
    explicit QtClock(QObject* parent = nullptr);
    ~QtClock() override;

    Timestamp now() const override;
    Timestamp monotonic() const override;

    TimerId scheduleOnce(Timestamp delayMillis, std::function<void()>) override;
    TimerId scheduleRepeating(Timestamp intervalMillis, std::function<void()>) override;
    void    cancel(TimerId) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace anytime::platform
