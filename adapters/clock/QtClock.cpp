// adapters/clock/QtClock.cpp

#include "adapters/clock/QtClock.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QTimer>

#include <unordered_map>

namespace anytime::platform {

struct QtClock::Impl {
    QElapsedTimer                            monotonic;
    std::unordered_map<TimerId, QTimer*>     timers;
    TimerId                                  nextId = 1;
};

QtClock::QtClock(QObject* parent) : QObject(parent), impl_(std::make_unique<Impl>())
{
    impl_->monotonic.start();
}

QtClock::~QtClock()
{
    for (auto& [id, timer] : impl_->timers) delete timer;
}

Timestamp QtClock::now() const { return QDateTime::currentMSecsSinceEpoch(); }

Timestamp QtClock::monotonic() const { return impl_->monotonic.elapsed(); }

TimerId QtClock::scheduleOnce(Timestamp delayMillis, std::function<void()> fn)
{
    const TimerId id = impl_->nextId++;

    auto* timer = new QTimer(this);
    timer->setSingleShot(true);
    timer->setInterval(static_cast<int>(delayMillis));

    connect(timer, &QTimer::timeout, this, [this, id, fn = std::move(fn)] {
        // Remove BEFORE invoking: the callback commonly reschedules, and it must
        // not find its own spent timer still registered.
        const auto it = impl_->timers.find(id);
        if (it != impl_->timers.end()) {
            it->second->deleteLater();
            impl_->timers.erase(it);
        }
        fn();
    });

    impl_->timers[id] = timer;
    timer->start();
    return id;
}

TimerId QtClock::scheduleRepeating(Timestamp intervalMillis, std::function<void()> fn)
{
    const TimerId id = impl_->nextId++;

    auto* timer = new QTimer(this);
    timer->setInterval(static_cast<int>(intervalMillis));
    connect(timer, &QTimer::timeout, this, std::move(fn));

    impl_->timers[id] = timer;
    timer->start();
    return id;
}

void QtClock::cancel(TimerId id)
{
    const auto it = impl_->timers.find(id);
    if (it == impl_->timers.end()) return;

    it->second->stop();
    it->second->deleteLater();
    impl_->timers.erase(it);
}

}  // namespace anytime::platform
