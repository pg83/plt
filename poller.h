#pragma once

#include <std/sys/types.h>

namespace stl {
    struct PollFD;
}

namespace plt {
    struct PollCallback {
        virtual void ready(stl::PollFD event) = 0;
    };

    struct TimerCallback {
        virtual void ready() = 0;
    };

    // File-descriptor registrations are one-shot: a registration is removed
    // before its callback runs and the callback re-arms if it wants more
    // events. arm() on an already-armed descriptor replaces the registration.
    // Timers are keyed by callback: timeout()/deadline() replace the pending
    // deadline for that callback, and cancel() guarantees the callback does
    // not run afterwards, even from a dispatch round already in progress.
    struct Poller {
        virtual void arm(stl::PollFD fd, PollCallback& callback) = 0;
        virtual void disarm(int fd) = 0;
        virtual void timeout(u64 microseconds, TimerCallback& callback) = 0;
        virtual void deadline(u64 monotonicMicroseconds, TimerCallback& callback) = 0;
        virtual void cancel(TimerCallback& callback) = 0;
    };
}
