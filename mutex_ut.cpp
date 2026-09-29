#include "mutex.h"

#include "fiber.h"
#include "poller.h"

#include <std/tst/ut.h>
#include <std/thr/poll_fd.h>
#include <std/thr/runable.h>
#include <std/mem/obj_pool.h>

using namespace plt;
using namespace stl;

namespace {
    struct InertPoller final: Poller {
        void arm(PollWaiter&) override {
        }

        void cancel(PollWaiter&) override {
        }

        void timeout(u64, TimerCallback&) override {
        }

        void deadline(u64, TimerCallback&) override {
        }

        void cancel(TimerCallback&) override {
        }

        void defer(TimerCallback&) override {
        }
    };

    bool probeFree(FiberMutex& mutex) {
        return !mutex.locked();
    }
}

STD_TEST_SUITE(FiberMutexSuite) {
    STD_TEST(TryLockTakesAndReleases) {
        ObjPool::Ref pool = ObjPool::fromMemory();
        InertPoller poller;
        Scheduler* const scheduler = Scheduler::create(*pool, poller);
        FiberMutex* const mutex = scheduler->createMutex(*pool);
        STD_INSIST(mutex->tryLock());
        STD_INSIST(!mutex->tryLock());
        mutex->unlock();
        STD_INSIST(mutex->tryLock());
        mutex->unlock();
    }

    STD_TEST(UncontendedLockDoesNotBlock) {
        ObjPool::Ref pool = ObjPool::fromMemory();
        InertPoller poller;
        Scheduler* const scheduler = Scheduler::create(*pool, poller);
        FiberMutex* const mutex = scheduler->createMutex(*pool);
        bool done = false;
        auto body = makeRunable([&] {
            mutex->lock();
            mutex->unlock();
            done = true;
        });
        alignas(16) static u8 bodyStack[lightFiberStack];
        scheduler->spawn(body, bodyStack, sizeof(bodyStack));
        STD_INSIST(done);
        STD_INSIST(probeFree(*mutex));
    }

    STD_TEST(WaitersResumeInFifoOrder) {
        ObjPool::Ref pool = ObjPool::fromMemory();
        InertPoller poller;
        Scheduler* const scheduler = Scheduler::create(*pool, poller);
        FiberMutex* const mutex = scheduler->createMutex(*pool);
        Fiber* owner = nullptr;
        int order = 0;
        int ownerAt = 0;
        int firstAt = 0;
        int secondAt = 0;
        auto ownerBody = makeRunable([&] {
            mutex->lock();
            owner = scheduler->current();
            owner->park();
            ownerAt = ++order;
            mutex->unlock();
        });
        auto firstBody = makeRunable([&] {
            mutex->lock();
            firstAt = ++order;
            mutex->unlock();
        });
        auto secondBody = makeRunable([&] {
            mutex->lock();
            secondAt = ++order;
            mutex->unlock();
        });
        alignas(16) static u8 ownerBodyStack[lightFiberStack];
        scheduler->spawn(ownerBody, ownerBodyStack, sizeof(ownerBodyStack));
        alignas(16) static u8 firstBodyStack[lightFiberStack];
        scheduler->spawn(firstBody, firstBodyStack, sizeof(firstBodyStack));
        alignas(16) static u8 secondBodyStack[lightFiberStack];
        scheduler->spawn(secondBody, secondBodyStack, sizeof(secondBodyStack));
        STD_INSIST(order == 0);
        owner->wake();
        STD_INSIST(ownerAt == 1);
        STD_INSIST(firstAt == 2);
        STD_INSIST(secondAt == 3);
        STD_INSIST(probeFree(*mutex));
    }

    STD_TEST(HandoffKeepsMutexHeld) {
        ObjPool::Ref pool = ObjPool::fromMemory();
        InertPoller poller;
        Scheduler* const scheduler = Scheduler::create(*pool, poller);
        FiberMutex* const mutex = scheduler->createMutex(*pool);
        Fiber* waiter = nullptr;
        bool waiterDone = false;
        STD_INSIST(mutex->tryLock());
        auto waiterBody = makeRunable([&] {
            waiter = scheduler->current();
            mutex->lock();
            waiter->park();
            mutex->unlock();
            waiterDone = true;
        });
        alignas(16) static u8 waiterBodyStack[lightFiberStack];
        scheduler->spawn(waiterBody, waiterBodyStack, sizeof(waiterBodyStack));
        // The waiter resumes inside unlock(), takes the mutex over and
        // blocks while still holding it.
        mutex->unlock();
        STD_INSIST(!waiterDone);
        STD_INSIST(!mutex->tryLock());
        waiter->wake();
        STD_INSIST(waiterDone);
        STD_INSIST(mutex->tryLock());
        mutex->unlock();
    }

    STD_TEST(LockGuardReleasesOnScopeExit) {
        ObjPool::Ref pool = ObjPool::fromMemory();
        InertPoller poller;
        Scheduler* const scheduler = Scheduler::create(*pool, poller);
        FiberMutex* const mutex = scheduler->createMutex(*pool);
        bool inner = false;
        auto body = makeRunable([&] {
            const LockGuard guard(*mutex);
            inner = mutex->heldByCurrent();
        });
        alignas(16) static u8 bodyStack[lightFiberStack];
        scheduler->spawn(body, bodyStack, sizeof(bodyStack));
        STD_INSIST(inner);
        STD_INSIST(probeFree(*mutex));
    }

    STD_TEST(HeldByCurrentSeesOnlyTheOwner) {
        ObjPool::Ref pool = ObjPool::fromMemory();
        InertPoller poller;
        Scheduler* const scheduler = Scheduler::create(*pool, poller);
        FiberMutex* const mutex = scheduler->createMutex(*pool);
        Fiber* owner = nullptr;
        bool ownerSees = false;
        bool strangerSees = true;
        auto ownerBody = makeRunable([&] {
            mutex->lock();
            owner = scheduler->current();
            ownerSees = mutex->heldByCurrent();
            owner->park();
            mutex->unlock();
        });
        auto strangerBody = makeRunable([&] {
            strangerSees = mutex->heldByCurrent();
        });
        alignas(16) static u8 ownerBodyStack[lightFiberStack];
        scheduler->spawn(ownerBody, ownerBodyStack, sizeof(ownerBodyStack));
        alignas(16) static u8 strangerBodyStack[lightFiberStack];
        scheduler->spawn(strangerBody, strangerBodyStack, sizeof(strangerBodyStack));
        // Outside any fiber there is no current to own anything.
        STD_INSIST(!mutex->heldByCurrent());
        STD_INSIST(ownerSees);
        STD_INSIST(!strangerSees);
        owner->wake();
        STD_INSIST(probeFree(*mutex));
    }

    STD_TEST(UnrelatedWakeDoesNotGrant) {
        ObjPool::Ref pool = ObjPool::fromMemory();
        InertPoller poller;
        Scheduler* const scheduler = Scheduler::create(*pool, poller);
        FiberMutex* const mutex = scheduler->createMutex(*pool);
        Fiber* waiter = nullptr;
        bool entered = false;
        STD_INSIST(mutex->tryLock());
        auto waiterBody = makeRunable([&] {
            waiter = scheduler->current();
            mutex->lock();
            entered = true;
            mutex->unlock();
        });
        alignas(16) static u8 waiterBodyStack[lightFiberStack];
        scheduler->spawn(waiterBody, waiterBodyStack, sizeof(waiterBodyStack));
        waiter->wake();
        STD_INSIST(!entered);
        mutex->unlock();
        STD_INSIST(entered);
        STD_INSIST(probeFree(*mutex));
    }
}
