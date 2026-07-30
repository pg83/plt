#include "poller.h"
#include "timer_queue.h"

#include <std/tst/ut.h>
#include <std/mem/obj_pool.h>

using namespace stl;
using namespace plt;

namespace {
    struct Counter final: public TimerCallback {
        void ready() override {
            ++count;
            order = ++*sequence;
        }

        u32 count = 0;
        u32 order = 0;
        u32* sequence = nullptr;
    };

    struct CancelOther final: public TimerCallback {
        CancelOther(TimerQueue& queue_, TimerCallback& other_)
            : queue(queue_)
            , other(other_)
        {
        }

        void ready() override {
            ++count;
            queue.cancel(other);
        }

        TimerQueue& queue;
        TimerCallback& other;
        u32 count = 0;
    };

    struct Rearm final: public TimerCallback {
        explicit Rearm(TimerQueue& queue_)
            : queue(queue_)
        {
        }

        void ready() override {
            ++count;
            queue.schedule(0, *this);
        }

        TimerQueue& queue;
        u32 count = 0;
    };
}

STD_TEST_SUITE(TimerQueue) {
    STD_TEST(DispatchesByDeadline) {
        ObjPool::Ref pool = ObjPool::fromMemory();
        TimerQueue queue(*pool);
        u32 sequence = 0;
        Counter early;
        Counter late;
        early.sequence = &sequence;
        late.sequence = &sequence;

        queue.schedule(10, late);
        queue.schedule(5, early);
        STD_INSIST(queue.nextDeadline() == 5);

        queue.dispatch(7);
        STD_INSIST(early.count == 1);
        STD_INSIST(late.count == 0);
        STD_INSIST(queue.nextDeadline() == 10);

        queue.dispatch(10);
        STD_INSIST(late.count == 1);
        STD_INSIST(queue.nextDeadline() == UINT64_MAX);
    }

    STD_TEST(DispatchOrderFollowsDeadlines) {
        ObjPool::Ref pool = ObjPool::fromMemory();
        TimerQueue queue(*pool);
        u32 sequence = 0;
        Counter first;
        Counter second;
        first.sequence = &sequence;
        second.sequence = &sequence;

        queue.schedule(20, second);
        queue.schedule(10, first);
        queue.dispatch(30);
        STD_INSIST(first.order == 1);
        STD_INSIST(second.order == 2);
    }

    STD_TEST(ScheduleReplacesDeadline) {
        ObjPool::Ref pool = ObjPool::fromMemory();
        TimerQueue queue(*pool);
        u32 sequence = 0;
        Counter counter;
        counter.sequence = &sequence;

        queue.schedule(5, counter);
        queue.schedule(50, counter);
        STD_INSIST(queue.nextDeadline() == 50);

        queue.dispatch(10);
        STD_INSIST(counter.count == 0);

        queue.dispatch(50);
        STD_INSIST(counter.count == 1);
    }

    STD_TEST(CancelPreventsDispatch) {
        ObjPool::Ref pool = ObjPool::fromMemory();
        TimerQueue queue(*pool);
        u32 sequence = 0;
        Counter counter;
        counter.sequence = &sequence;

        queue.schedule(5, counter);
        queue.cancel(counter);
        queue.dispatch(10);
        STD_INSIST(counter.count == 0);
        STD_INSIST(queue.nextDeadline() == UINT64_MAX);
    }

    STD_TEST(CancelFromCallbackStopsDueTimer) {
        ObjPool::Ref pool = ObjPool::fromMemory();
        TimerQueue queue(*pool);
        u32 sequence = 0;
        Counter cancelled;
        cancelled.sequence = &sequence;
        CancelOther canceller(queue, cancelled);

        queue.schedule(1, canceller);
        queue.schedule(2, cancelled);
        queue.dispatch(10);
        STD_INSIST(canceller.count == 1);
        STD_INSIST(cancelled.count == 0);
    }

    STD_TEST(RearmFromCallbackWaitsForNextRound) {
        ObjPool::Ref pool = ObjPool::fromMemory();
        TimerQueue queue(*pool);
        Rearm rearm(queue);

        queue.schedule(0, rearm);
        queue.dispatch(100);
        STD_INSIST(rearm.count == 1);

        queue.dispatch(100);
        STD_INSIST(rearm.count == 2);
        queue.cancel(rearm);
    }
}
