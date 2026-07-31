#pragma once

#include <std/sys/types.h>

namespace stl {
    class ObjPool;
    struct Runable;
}

namespace plt {
    struct Poller;

    // A single-threaded cooperative fiber scheduler married to a Poller.
    // Fibers run on the platform thread: a blocked fiber resumes inside the
    // poller callback that made it runnable and switches back before the
    // loop continues, so fibers, callbacks and the event loop interleave
    // freely. Everything a fiber touches lives on its own stack, which lets
    // deeply nested code block on I/O without stopping the loop.
    struct Scheduler {
        // Runs entry immediately on a fresh stack until it first blocks or
        // returns. entry must stay alive until the fiber finishes.
        virtual void spawn(stl::Runable& entry) = 0;

        // The calls below block the calling fiber only and must not be used
        // outside one. false means the wait timed out; a timeout of 0 waits
        // without a deadline.
        virtual bool awaitReadable(int fd, u64 timeoutUs) = 0;
        virtual bool awaitWritable(int fd, u64 timeoutUs) = 0;
        virtual void sleep(u64 timeoutUs) = 0;
        virtual void yield() = 0;
        virtual bool inFiber() const = 0;

        static Scheduler* create(stl::ObjPool& owner, Poller& poller);
    };
}
