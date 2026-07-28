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

    struct Poller {
        virtual void arm(stl::PollFD fd, PollCallback& callback) = 0;
        virtual void disarm(int fd) = 0;
        virtual void timeout(u64 microseconds, TimerCallback& callback) = 0;
        virtual void deadline(u64 monotonicMicroseconds, TimerCallback& callback) = 0;
        virtual void cancel(TimerCallback& callback) = 0;
    };
}
