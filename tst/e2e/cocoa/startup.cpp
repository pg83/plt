#define main desktopMain
#include "desktop.cpp"
#undef main

#include "loop_wake.h"
#include "poller.h"

#include <std/sys/throw.h>
#include <std/thr/poll_fd.h>

namespace {
    struct Probe final: public TimerCallback, public PollCallback {
        void ready() override;
        void ready(PollFD) override;
    };
}

void Probe::ready() {
}

void Probe::ready(PollFD) {
}

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    int pipes[2];
    STD_INSIST(pipe(pipes) == 0);
    bool failed = false;
    try {
        auto owner = ObjPool::fromMemory();
        Platform* const platform = Platform::create(*owner);
        Probe probe;
        platform->createLoopWake(*owner, probe)->signal();
        PollWaiter waiter;
        waiter.fd = {.fd = pipes[0], .flags = PollFlag::In};
        waiter.callback = &probe;
        platform->poller()->arm(waiter);
        platform->poller()->cancel(waiter);
    } catch (Exception& error) {
        const StringView description = error.description();
        printf("RESOURCE FAILURE %.*s\n", (int)description.length(), description.data());
        failed = true;
    }
    close(pipes[0]);
    close(pipes[1]);
    STD_INSIST(failed);
    return desktopMain();
}
