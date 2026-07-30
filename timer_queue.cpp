#include "timer_queue.h"

#include "poller.h"

#include <cstdint>

using namespace stl;
using namespace plt;

TimerEntry::TimerEntry(TimerCallback* callback_, u64 deadline_, u64 sequence_)
    : callback(callback_)
    , deadline(deadline_)
    , sequence(sequence_)
    , nextDue(nullptr)
{
}

bool TimerQueue::Order::cmp(void* a, void* b) const noexcept {
    const TimerEntry* const left = (const TimerEntry*)(a);
    const TimerEntry* const right = (const TimerEntry*)(b);
    if (left->deadline != right->deadline) {
        return left->deadline < right->deadline;
    }
    return left->sequence < right->sequence;
}

TimerQueue::TimerQueue(ObjPool& owner)
    : entries_(&owner)
{
}

TimerEntry* TimerQueue::takeScheduled(TimerCallback& callback) {
    TimerEntry* found = nullptr;
    order_.visit([&found, &callback](TreapNode* node) {
        TimerEntry* const entry = (TimerEntry*)(node);
        if (entry->callback == &callback) {
            found = entry;
        }
    });
    if (found != nullptr) {
        order_.remove(found);
        return found;
    }
    for (TimerEntry** current = &due_; *current != nullptr; current = &(*current)->nextDue) {
        if ((*current)->callback == &callback) {
            found = *current;
            *current = found->nextDue;
            found->nextDue = nullptr;
            return found;
        }
    }
    return nullptr;
}

void TimerQueue::schedule(u64 deadline, TimerCallback& callback) {
    TimerEntry* entry = takeScheduled(callback);
    if (entry == nullptr) {
        entry = entries_.make(&callback, deadline, nextSequence_++);
    } else {
        entry->deadline = deadline;
        entry->sequence = nextSequence_++;
    }
    order_.insert(entry);
}

void TimerQueue::cancel(TimerCallback& callback) {
    if (TimerEntry* const entry = takeScheduled(callback)) {
        entries_.release(entry);
    }
}

void TimerQueue::dispatch(u64 now) {
    // Collect the due entries first so timers armed from a callback wait for
    // the next round instead of firing while this one is still running.
    TimerEntry** tail = &due_;
    while (*tail != nullptr) {
        tail = &(*tail)->nextDue;
    }
    while (TimerEntry* const entry = (TimerEntry*)(order_.min())) {
        if (entry->deadline > now) {
            break;
        }
        order_.remove(entry);
        entry->nextDue = nullptr;
        *tail = entry;
        tail = &entry->nextDue;
    }
    while (due_ != nullptr) {
        TimerEntry* const entry = due_;
        due_ = entry->nextDue;
        TimerCallback* const callback = entry->callback;
        entries_.release(entry);
        callback->ready();
    }
}

u64 TimerQueue::nextDeadline() const {
    const TimerEntry* const entry = (const TimerEntry*)(order_.min());
    return entry == nullptr ? UINT64_MAX : entry->deadline;
}
