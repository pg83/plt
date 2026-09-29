#include "fiber.h"
#include "platform.h"
#include "poller.h"

#include <std/tst/ut.h>
#include <std/thr/poll_fd.h>
#include <std/thr/runable.h>
#include <std/mem/obj_pool.h>

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

using namespace plt;
using namespace stl;

namespace {
    struct ManualPoller final: Poller {
        void arm(PollWaiter& waiter) override {
            armedFd = waiter.fd.fd;
            fdCallback = waiter.callback;
        }

        void cancel(PollWaiter&) override {
            armedFd = -1;
            fdCallback = nullptr;
        }

        void timeout(u64, TimerCallback& callback) override {
            timer = &callback;
        }

        void deadline(u64, TimerCallback& callback) override {
            timer = &callback;
        }

        void cancel(TimerCallback&) override {
            timer = nullptr;
        }

        void defer(TimerCallback& callback) override {
            timer = &callback;
        }

        void fireFd() {
            PollCallback* const callback = fdCallback;
            fdCallback = nullptr;
            armedFd = -1;
            callback->ready(PollFD{});
        }

        void fireTimer() {
            TimerCallback* const callback = timer;
            timer = nullptr;
            callback->ready();
        }

        int armedFd = -1;
        PollCallback* fdCallback = nullptr;
        TimerCallback* timer = nullptr;
    };
}

STD_TEST_SUITE(FiberScheduler) {
    STD_TEST(SpawnRunsImmediately) {
        ObjPool::Ref pool = ObjPool::fromMemory();
        ManualPoller poller;
        Scheduler* const scheduler = Scheduler::create(*pool, poller);
        int steps = 0;
        auto body = makeRunable([&] {
            ++steps;
        });
        alignas(16) static u8 bodyStack[lightFiberStack];
        scheduler->spawn(body, bodyStack, sizeof(bodyStack));
        STD_INSIST(steps == 1);
        STD_INSIST(scheduler->current() == nullptr);
    }

    STD_TEST(AwaitResumesOnFd) {
        ObjPool::Ref pool = ObjPool::fromMemory();
        ManualPoller poller;
        Scheduler* const scheduler = Scheduler::create(*pool, poller);
        int phase = 0;
        bool ready = false;
        auto body = makeRunable([&] {
            phase = 1;
            ready = scheduler->awaitReadable(7, 1000);
            phase = 2;
        });
        alignas(16) static u8 bodyStack[lightFiberStack];
        scheduler->spawn(body, bodyStack, sizeof(bodyStack));
        STD_INSIST(phase == 1);
        STD_INSIST(poller.armedFd == 7);
        poller.fireFd();
        STD_INSIST(phase == 2);
        STD_INSIST(ready);
        STD_INSIST(poller.timer == nullptr);
    }

    STD_TEST(AwaitTimesOut) {
        ObjPool::Ref pool = ObjPool::fromMemory();
        ManualPoller poller;
        Scheduler* const scheduler = Scheduler::create(*pool, poller);
        bool ready = true;
        bool complete = false;
        auto body = makeRunable([&] {
            ready = scheduler->awaitReadable(7, 1000);
            complete = true;
        });
        alignas(16) static u8 bodyStack[lightFiberStack];
        scheduler->spawn(body, bodyStack, sizeof(bodyStack));
        poller.fireTimer();
        STD_INSIST(complete);
        STD_INSIST(!ready);
        STD_INSIST(poller.fdCallback == nullptr);
    }

    STD_TEST(TimedParkAndInterleave) {
        ObjPool::Ref pool = ObjPool::fromMemory();
        ManualPoller poller;
        Scheduler* const scheduler = Scheduler::create(*pool, poller);
        int order = 0;
        int firstAt = 0;
        int loopAt = 0;
        auto body = makeRunable([&] {
            firstAt = ++order;
            scheduler->current()->parkFor(1000);
            firstAt = ++order;
        });
        alignas(16) static u8 bodyStack[lightFiberStack];
        scheduler->spawn(body, bodyStack, sizeof(bodyStack));
        // The loop runs while the fiber waits.
        loopAt = ++order;
        poller.fireTimer();
        STD_INSIST(loopAt == 2);
        STD_INSIST(firstAt == 3);
    }

    STD_TEST(ParkAndWake) {
        ObjPool::Ref pool = ObjPool::fromMemory();
        ManualPoller poller;
        Scheduler* const scheduler = Scheduler::create(*pool, poller);
        Fiber* handle = nullptr;
        int phase = 0;
        auto body = makeRunable([&] {
            handle = scheduler->current();
            phase = 1;
            scheduler->current()->park();
            phase = 2;
            scheduler->current()->park();
            phase = 3;
        });
        alignas(16) static u8 bodyStack[lightFiberStack];
        scheduler->spawn(body, bodyStack, sizeof(bodyStack));
        STD_INSIST(phase == 1);
        handle->wake();
        STD_INSIST(phase == 2);
        handle->wake();
        STD_INSIST(phase == 3);
    }

    STD_TEST(WakeBeforeParkIsRemembered) {
        ObjPool::Ref pool = ObjPool::fromMemory();
        ManualPoller poller;
        Scheduler* const scheduler = Scheduler::create(*pool, poller);
        Fiber* handle = nullptr;
        bool woken = false;
        auto body = makeRunable([&] {
            handle = scheduler->current();
            // Blocked on a descriptor, not parked: the wake below must be
            // remembered, not resume the wait.
            scheduler->awaitReadable(0, 1000);
            scheduler->current()->park();
            woken = true;
        });
        alignas(16) static u8 bodyStack[lightFiberStack];
        scheduler->spawn(body, bodyStack, sizeof(bodyStack));
        handle->wake();
        STD_INSIST(!woken);
        poller.fireTimer();
        STD_INSIST(woken);
    }

    STD_TEST(NestedSpawn) {
        ObjPool::Ref pool = ObjPool::fromMemory();
        ManualPoller poller;
        Scheduler* const scheduler = Scheduler::create(*pool, poller);
        bool innerBlocked = false;
        bool innerDone = false;
        bool outerDone = false;
        auto inner = makeRunable([&] {
            innerBlocked = true;
            scheduler->current()->parkFor(1000);
            innerDone = true;
        });
        auto outer = makeRunable([&] {
            alignas(16) static u8 innerStack[lightFiberStack];
        scheduler->spawn(inner, innerStack, sizeof(innerStack));
            STD_INSIST(scheduler->current() != nullptr);
            outerDone = true;
        });
        alignas(16) static u8 outerStack[lightFiberStack];
        scheduler->spawn(outer, outerStack, sizeof(outerStack));
        STD_INSIST(innerBlocked);
        STD_INSIST(outerDone);
        STD_INSIST(!innerDone);
        poller.fireTimer();
        STD_INSIST(innerDone);
    }

    STD_TEST(ReleaseParkedFiberReturnsTheStack) {
        ObjPool::Ref pool = ObjPool::fromMemory();
        ManualPoller poller;
        Scheduler* const scheduler = Scheduler::create(*pool, poller);
        Fiber* handle = nullptr;
        bool resumed = false;
        auto body = makeRunable([&] {
            handle = scheduler->current();
            scheduler->current()->park();
            resumed = true;
        });
        alignas(16) static u8 bodyStack[lightFiberStack];
        scheduler->spawn(body, bodyStack, sizeof(bodyStack));
        // A plain park armed nothing, so the release stands alone: no
        // poller reference is left to collect anything later.
        handle->release();
        STD_INSIST(poller.timer == nullptr);
        STD_INSIST(poller.fdCallback == nullptr);
        // The stack is the caller's again: a fresh fiber may live there.
        bool reused = false;
        auto next = makeRunable([&] {
            reused = true;
        });
        scheduler->spawn(next, bodyStack, sizeof(bodyStack));
        STD_INSIST(reused);
        STD_INSIST(!resumed);
    }

    STD_TEST(ReleaseTimedParkBuriesItselfOnTheTimer) {
        ObjPool::Ref pool = ObjPool::fromMemory();
        ManualPoller poller;
        Scheduler* const scheduler = Scheduler::create(*pool, poller);
        Fiber* handle = nullptr;
        bool resumed = false;
        auto body = makeRunable([&] {
            handle = scheduler->current();
            scheduler->current()->parkFor(1000);
            resumed = true;
        });
        alignas(16) static u8 bodyStack[lightFiberStack];
        scheduler->spawn(body, bodyStack, sizeof(bodyStack));
        // The deadline stays armed through the release; firing it must
        // bury the tombstone instead of resuming a freed fiber.
        handle->release();
        STD_INSIST(poller.timer != nullptr);
        poller.fireTimer();
        STD_INSIST(!resumed);
        bool reused = false;
        auto next = makeRunable([&] {
            reused = true;
        });
        scheduler->spawn(next, bodyStack, sizeof(bodyStack));
        STD_INSIST(reused);
        STD_INSIST(!resumed);
    }

    STD_TEST(ReleaseAwaitBuriesItselfOnReadiness) {
        ObjPool::Ref pool = ObjPool::fromMemory();
        ManualPoller poller;
        Scheduler* const scheduler = Scheduler::create(*pool, poller);
        Fiber* handle = nullptr;
        bool resumed = false;
        auto body = makeRunable([&] {
            handle = scheduler->current();
            scheduler->awaitReadable(7, 0);
            resumed = true;
        });
        alignas(16) static u8 bodyStack[lightFiberStack];
        scheduler->spawn(body, bodyStack, sizeof(bodyStack));
        STD_INSIST(poller.armedFd == 7);
        // The waiter node lives inside the released handle, not on the
        // freed stack, so the readiness below walks valid memory.
        handle->release();
        poller.fireFd();
        STD_INSIST(!resumed);
        bool reused = false;
        auto next = makeRunable([&] {
            reused = true;
        });
        scheduler->spawn(next, bodyStack, sizeof(bodyStack));
        STD_INSIST(reused);
        STD_INSIST(!resumed);
    }

    STD_TEST(ReleaseAwaitWithDeadlineTimerFiresFirst) {
        ObjPool::Ref pool = ObjPool::fromMemory();
        ManualPoller poller;
        Scheduler* const scheduler = Scheduler::create(*pool, poller);
        Fiber* handle = nullptr;
        bool resumed = false;
        auto body = makeRunable([&] {
            handle = scheduler->current();
            scheduler->awaitReadable(7, 1000);
            resumed = true;
        });
        alignas(16) static u8 bodyStack[lightFiberStack];
        scheduler->spawn(body, bodyStack, sizeof(bodyStack));
        STD_INSIST(poller.armedFd == 7);
        STD_INSIST(poller.timer != nullptr);
        // Both references are armed; whichever fires first must take the
        // sibling down with the tombstone or the second would dispatch
        // into freed memory.
        handle->release();
        poller.fireTimer();
        STD_INSIST(!resumed);
        STD_INSIST(poller.fdCallback == nullptr);
        STD_INSIST(poller.armedFd == -1);
    }

    STD_TEST(ReleaseAwaitWithDeadlineFdFiresFirst) {
        ObjPool::Ref pool = ObjPool::fromMemory();
        ManualPoller poller;
        Scheduler* const scheduler = Scheduler::create(*pool, poller);
        Fiber* handle = nullptr;
        bool resumed = false;
        auto body = makeRunable([&] {
            handle = scheduler->current();
            scheduler->awaitReadable(7, 1000);
            resumed = true;
        });
        alignas(16) static u8 bodyStack[lightFiberStack];
        scheduler->spawn(body, bodyStack, sizeof(bodyStack));
        handle->release();
        poller.fireFd();
        STD_INSIST(!resumed);
        STD_INSIST(poller.timer == nullptr);
    }

    STD_TEST(FinishedFibersRecycleTheirControlBlocks) {
        ObjPool::Ref pool = ObjPool::fromMemory();
        ManualPoller poller;
        Scheduler* const scheduler = Scheduler::create(*pool, poller);
        // Every finish frees the control block in the resume epilogue;
        // churning one stack through many lives exercises the recycling
        // path end to end.
        int lives = 0;
        auto body = makeRunable([&] {
            ++lives;
        });
        alignas(16) static u8 bodyStack[lightFiberStack];
        for (int spawnRound = 0; spawnRound < 1000; ++spawnRound) {
            scheduler->spawn(body, bodyStack, sizeof(bodyStack));
        }
        STD_INSIST(lives == 1000);
    }

    STD_TEST(OwnedFiberHandleSurvivesNaturalFinish) {
        ObjPool::Ref pool = ObjPool::fromMemory();
        ManualPoller poller;
        Scheduler* const scheduler = Scheduler::create(*pool, poller);
        ObjPool* const owner = ObjPool::fromMemoryRaw();
        int runs = 0;
        auto body = makeRunable([&] {
            ++runs;
        });
        Fiber* const fiber = scheduler->create(*owner, body);
        STD_INSIST(runs == 1);
        STD_INSIST(scheduler->current() == nullptr);
        fiber->wake();
        STD_INSIST(runs == 1);
        delete owner;
    }

    STD_TEST(OwnedPoolReleasesParkedFiber) {
        ObjPool::Ref pool = ObjPool::fromMemory();
        ManualPoller poller;
        Scheduler* const scheduler = Scheduler::create(*pool, poller);
        ObjPool* const owner = ObjPool::fromMemoryRaw();
        bool resumed = false;
        auto body = makeRunable([&] {
            scheduler->current()->park();
            resumed = true;
        });
        scheduler->create(*owner, body);
        delete owner;
        STD_INSIST(!resumed);
        STD_INSIST(poller.timer == nullptr);
        STD_INSIST(poller.fdCallback == nullptr);
    }

    STD_TEST(OwnedPoolReleasesAwaitedFiber) {
        ObjPool::Ref pool = ObjPool::fromMemory();
        ManualPoller poller;
        Scheduler* const scheduler = Scheduler::create(*pool, poller);
        ObjPool* const owner = ObjPool::fromMemoryRaw();
        bool resumed = false;
        auto body = makeRunable([&] {
            scheduler->awaitReadable(7, 0);
            resumed = true;
        });
        scheduler->create(*owner, body);
        STD_INSIST(poller.armedFd == 7);
        delete owner;
        poller.fireFd();
        STD_INSIST(!resumed);
    }

    STD_TEST(OwnedPoolReleasesYieldedFiber) {
        ObjPool::Ref pool = ObjPool::fromMemory();
        ManualPoller poller;
        Scheduler* const scheduler = Scheduler::create(*pool, poller);
        ObjPool* const owner = ObjPool::fromMemoryRaw();
        bool resumed = false;
        auto body = makeRunable([&] {
            scheduler->yield();
            resumed = true;
        });
        scheduler->create(*owner, body);
        STD_INSIST(poller.timer != nullptr);
        delete owner;
        poller.fireTimer();
        STD_INSIST(!resumed);
    }
}

namespace {
    // The system platform: Cocoa on macOS, Wayland on Linux. The Wayland
    // backend needs a compositor, so without a display these tests skip
    // rather than fail; the fake-compositor integration suite covers that
    // path.
    Platform* systemPlatform(ObjPool& pool) {
#if !defined(__APPLE__)
        if (getenv("WAYLAND_DISPLAY") == nullptr) {
            return nullptr;
        }
#endif
        return Platform::create(pool);
    }

    void makeNonblocking(int fd) {
        const int flags = fcntl(fd, F_GETFL, 0);
        STD_INSIST(flags >= 0);
        STD_INSIST(fcntl(fd, F_SETFL, flags | O_NONBLOCK) >= 0);
    }

    void makePipe(int* fds) {
        STD_INSIST(pipe(fds) == 0);
        makeNonblocking(fds[0]);
        makeNonblocking(fds[1]);
    }

    // Fibers park on real descriptors here, so a broken poller means a
    // wait that never resumes; every await carries this deadline to turn
    // a hang into a failed assertion instead.
    constexpr u64 waitDeadline = 5'000'000;

    // Counts fiber completions and stops the platform loop after the last
    // one; every test drives its fibers to completion before asserting.
    struct StopAfter {
        StopAfter(Platform& platform_, int remaining_)
            : platform(platform_)
            , remaining(remaining_)
        {
        }

        void done() {
            if (--remaining == 0) {
                platform.stop();
            }
        }

        Platform& platform;
        int remaining;
    };
}

STD_TEST_SUITE(FiberSchedulerPlatform) {
    STD_TEST(PipePingPongAcrossFibers) {
        ObjPool::Ref pool = ObjPool::fromMemory();
        Platform* const platform = systemPlatform(*pool);
        if (platform == nullptr) {
            return;
        }
        Scheduler* const scheduler = platform->scheduler();
        constexpr int rounds = 25;
        int forward[2];
        int backward[2];
        makePipe(forward);
        makePipe(backward);
        StopAfter stop(*platform, 2);
        int sent = 0;
        int bounced = 0;
        bool aborted = false;
        auto initiator = makeRunable([&] {
            for (int round = 0; round < rounds; ++round) {
                u8 token = (u8)(round);
                if (write(forward[1], &token, 1) != 1 || !scheduler->awaitReadable(backward[0], waitDeadline)) {
                    aborted = true;
                    break;
                }
                u8 echo = 0xff;
                if (read(backward[0], &echo, 1) != 1 || echo != token) {
                    aborted = true;
                    break;
                }
                ++sent;
            }
            stop.done();
        });
        auto responder = makeRunable([&] {
            for (int round = 0; round < rounds; ++round) {
                if (!scheduler->awaitReadable(forward[0], waitDeadline)) {
                    aborted = true;
                    break;
                }
                u8 token = 0xff;
                if (read(forward[0], &token, 1) != 1 || write(backward[1], &token, 1) != 1) {
                    aborted = true;
                    break;
                }
                ++bounced;
            }
            stop.done();
        });
        alignas(16) static u8 initiatorStack[lightFiberStack];
        alignas(16) static u8 responderStack[lightFiberStack];
        scheduler->spawn(initiator, initiatorStack, sizeof(initiatorStack));
        scheduler->spawn(responder, responderStack, sizeof(responderStack));
        platform->run();
        STD_INSIST(!aborted);
        STD_INSIST(sent == rounds);
        STD_INSIST(bounced == rounds);
        close(forward[0]);
        close(forward[1]);
        close(backward[0]);
        close(backward[1]);
    }

    STD_TEST(PtyChildOutputWakesReader) {
        ObjPool::Ref pool = ObjPool::fromMemory();
        Platform* const platform = systemPlatform(*pool);
        if (platform == nullptr) {
            return;
        }
        Scheduler* const scheduler = platform->scheduler();
        // The terminal's exact shape: a child process writes into the pty
        // slave in bursts while a fiber drains the master through the
        // platform poller.
        constexpr size_t chunkSize = 128;
        constexpr size_t chunks = 8;
        constexpr size_t total = chunkSize * chunks;
        int master = -1;
        int slave = -1;
        STD_INSIST(openpty(&master, &slave, nullptr, nullptr, nullptr) == 0);
        makeNonblocking(master);
        const pid_t child = fork();
        STD_INSIST(child >= 0);
        if (child == 0) {
            // Only async-signal-safe calls between fork and _exit.
            close(master);
            u8 chunk[chunkSize];
            for (size_t index = 0; index < chunks; ++index) {
                memset(chunk, 'a' + (int)(index), sizeof(chunk));
                size_t written = 0;
                while (written < sizeof(chunk)) {
                    const ssize_t count = write(slave, chunk + written, sizeof(chunk) - written);
                    if (count < 0) {
                        _exit(1);
                    }
                    written += (size_t)(count);
                }
                usleep(2000);
            }
            _exit(0);
        }
        close(slave);
        StopAfter stop(*platform, 1);
        size_t received = 0;
        size_t misordered = 0;
        bool eof = false;
        auto reader = makeRunable([&] {
            u8 buffer[512];
            while (received < total) {
                const ssize_t count = read(master, buffer, sizeof(buffer));
                if (count > 0) {
                    for (ssize_t index = 0; index < count; ++index) {
                        misordered += buffer[index] != 'a' + (int)((received + (size_t)(index)) / chunkSize);
                    }
                    received += (size_t)(count);
                    continue;
                }
                if (count == 0 || (count < 0 && errno == EIO)) {
                    eof = true;
                    break;
                }
                if (count < 0 && errno == EINTR) {
                    continue;
                }
                if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    if (!scheduler->awaitReadable(master, waitDeadline)) {
                        break;
                    }
                    continue;
                }
                break;
            }
            stop.done();
        });
        alignas(16) static u8 readerStack[lightFiberStack];
        scheduler->spawn(reader, readerStack, sizeof(readerStack));
        platform->run();
        int status = -1;
        STD_INSIST(waitpid(child, &status, 0) == child);
        STD_INSIST(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        STD_INSIST(!eof);
        STD_INSIST(received == total);
        STD_INSIST(misordered == 0);
        close(master);
    }

    STD_TEST(SharedDescriptorServesMixedWaiters) {
        ObjPool::Ref pool = ObjPool::fromMemory();
        Platform* const platform = systemPlatform(*pool);
        if (platform == nullptr) {
            return;
        }
        Scheduler* const scheduler = platform->scheduler();
        int sockets[2];
        STD_INSIST(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
        makeNonblocking(sockets[0]);
        makeNonblocking(sockets[1]);
        // Three read waiters and one write waiter park on one descriptor:
        // the write side resumes on its own, then feeds the readers, and a
        // single readiness must serve all three of them.
        constexpr int readerCount = 3;
        StopAfter stop(*platform, readerCount + 1);
        int awoken = 0;
        bool writable = false;
        auto writer = makeRunable([&] {
            writable = scheduler->awaitWritable(sockets[0], waitDeadline);
            if (writable) {
                const u8 byte = 42;
                STD_INSIST(write(sockets[1], &byte, 1) == 1);
            }
            stop.done();
        });
        auto reader = makeRunable([&] {
            awoken += scheduler->awaitReadable(sockets[0], waitDeadline);
            stop.done();
        });
        alignas(16) static u8 writerStack[lightFiberStack];
        alignas(16) static u8 readerStacks[readerCount][lightFiberStack];
        for (int index = 0; index < readerCount; ++index) {
            scheduler->spawn(reader, readerStacks[index], sizeof(readerStacks[index]));
        }
        scheduler->spawn(writer, writerStack, sizeof(writerStack));
        platform->run();
        STD_INSIST(writable);
        STD_INSIST(awoken == readerCount);
        close(sockets[0]);
        close(sockets[1]);
    }

    STD_TEST(TimedOutWaiterLeavesDataForTheNext) {
        ObjPool::Ref pool = ObjPool::fromMemory();
        Platform* const platform = systemPlatform(*pool);
        if (platform == nullptr) {
            return;
        }
        Scheduler* const scheduler = platform->scheduler();
        int pipes[2];
        makePipe(pipes);
        StopAfter stop(*platform, 1);
        bool firstWait = true;
        bool secondWait = false;
        u8 seen = 0;
        auto body = makeRunable([&] {
            // The pipe stays quiet: this wait must time out through the
            // real run-loop timer and cancel its waiter.
            firstWait = scheduler->awaitReadable(pipes[0], 20'000);
            const u8 byte = 7;
            STD_INSIST(write(pipes[1], &byte, 1) == 1);
            // Data written after the timeout belongs to the next wait.
            secondWait = scheduler->awaitReadable(pipes[0], waitDeadline);
            if (secondWait) {
                STD_INSIST(read(pipes[0], &seen, 1) == 1);
            }
            stop.done();
        });
        alignas(16) static u8 bodyStack[lightFiberStack];
        scheduler->spawn(body, bodyStack, sizeof(bodyStack));
        platform->run();
        STD_INSIST(!firstWait);
        STD_INSIST(secondWait);
        STD_INSIST(seen == 7);
        close(pipes[0]);
        close(pipes[1]);
    }

    STD_TEST(YieldingFiberDoesNotStarveReader) {
        ObjPool::Ref pool = ObjPool::fromMemory();
        Platform* const platform = systemPlatform(*pool);
        if (platform == nullptr) {
            return;
        }
        Scheduler* const scheduler = platform->scheduler();
        int pipes[2];
        makePipe(pipes);
        const u8 byte = 1;
        STD_INSIST(write(pipes[1], &byte, 1) == 1);
        StopAfter stop(*platform, 2);
        // The data is already pending, so the reader parks exactly once;
        // the yielder hammers the loop the whole time and must not keep
        // the readiness callback from running.
        constexpr int yieldCap = 1000;
        int yieldsUntilRead = -1;
        bool readerReady = false;
        auto yielder = makeRunable([&] {
            for (int spin = 0; spin < yieldCap && !readerReady; ++spin) {
                scheduler->yield();
                yieldsUntilRead = spin + 1;
            }
            stop.done();
        });
        auto reader = makeRunable([&] {
            readerReady = scheduler->awaitReadable(pipes[0], waitDeadline);
            stop.done();
        });
        alignas(16) static u8 yielderStack[lightFiberStack];
        alignas(16) static u8 readerStack[lightFiberStack];
        scheduler->spawn(yielder, yielderStack, sizeof(yielderStack));
        scheduler->spawn(reader, readerStack, sizeof(readerStack));
        platform->run();
        STD_INSIST(readerReady);
        STD_INSIST(yieldsUntilRead >= 1);
        STD_INSIST(yieldsUntilRead < yieldCap);
        close(pipes[0]);
        close(pipes[1]);
    }

    STD_TEST(TimedParkDeadlinesResumeInOrder) {
        ObjPool::Ref pool = ObjPool::fromMemory();
        Platform* const platform = systemPlatform(*pool);
        if (platform == nullptr) {
            return;
        }
        Scheduler* const scheduler = platform->scheduler();
        StopAfter stop(*platform, 2);
        int order = 0;
        int longAt = 0;
        int shortAt = 0;
        // Spawned long-sleeper first; deadlines, not spawn order, decide
        // who resumes first through the shared run-loop timer.
        auto longSleeper = makeRunable([&] {
            scheduler->current()->parkFor(60'000);
            longAt = ++order;
            stop.done();
        });
        auto shortSleeper = makeRunable([&] {
            scheduler->current()->parkFor(15'000);
            shortAt = ++order;
            stop.done();
        });
        alignas(16) static u8 longStack[lightFiberStack];
        alignas(16) static u8 shortStack[lightFiberStack];
        scheduler->spawn(longSleeper, longStack, sizeof(longStack));
        scheduler->spawn(shortSleeper, shortStack, sizeof(shortStack));
        platform->run();
        STD_INSIST(shortAt == 1);
        STD_INSIST(longAt == 2);
    }
}
