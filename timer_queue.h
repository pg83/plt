#pragma once

#include <std/sys/types.h>
#include <std/map/treap.h>
#include <std/map/treap_node.h>
#include <std/mem/obj_list.h>

namespace stl {
    class ObjPool;
}

namespace plt {
    struct TimerCallback;

    struct TimerEntry final: public stl::TreapNode {
        TimerEntry(TimerCallback* callback, u64 deadline, u64 sequence);

        TimerCallback* callback;
        u64 deadline;
        u64 sequence;
        TimerEntry* nextDue;
    };

    // Deadline-ordered timers with the Poller contract: scheduling a callback
    // replaces its previous deadline, cancel() also stops a timer that is due
    // in the current dispatch round, and a timer armed from a callback never
    // fires in the round which armed it.
    class TimerQueue {
        struct Order final: public stl::Treap {
            bool cmp(void* a, void* b) const noexcept override;
        };

        TimerEntry* takeScheduled(TimerCallback& callback);

        Order order_;
        stl::ObjList<TimerEntry> entries_;
        TimerEntry* due_ = nullptr;
        u64 nextSequence_ = 1;

    public:
        explicit TimerQueue(stl::ObjPool& owner);

        void schedule(u64 deadline, TimerCallback& callback);
        void cancel(TimerCallback& callback);
        // Invokes every callback whose deadline is at or before now.
        void dispatch(u64 now);
        u64 nextDeadline() const;
    };
}
