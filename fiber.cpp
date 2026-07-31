#include "fiber.h"

#include "poller.h"

#include <std/lib/buffer.h>
#include <std/thr/context.h>
#include <std/thr/poll_fd.h>
#include <std/thr/runable.h>
#include <std/mem/obj_pool.h>

#include <alloca.h>

using namespace plt;
using namespace stl;

namespace {
    constexpr size_t fiberStackSize = 256 * 1024;

    struct SchedulerImpl;

    struct FiberImpl final: public PollCallback, public TimerCallback, public Runable {
        FiberImpl(SchedulerImpl& scheduler, Runable& entry);

        void ready(PollFD event) override;
        void ready() override;
        void run() override;

        void block();

        SchedulerImpl& scheduler;
        Runable& entry;
        Buffer stackStorage;
        Buffer contextStorage;
        Context* context = nullptr;
        Context* resumeTo = nullptr;
        bool fdReady = false;
        bool timerFired = false;
        bool finished = false;
    };

    struct SchedulerImpl final: public Scheduler {
        explicit SchedulerImpl(Poller& poller);

        void spawn(Runable& entry) override;
        bool awaitReadable(int fd, u64 timeoutUs) override;
        bool awaitWritable(int fd, u64 timeoutUs) override;
        void sleep(u64 timeoutUs) override;
        void yield() override;
        bool inFiber() const override;

        bool awaitFd(int fd, u32 flags, u64 timeoutUs);
        void resume(FiberImpl& fiber);

        Poller& poller;
        FiberImpl* active = nullptr;
    };
}

FiberImpl::FiberImpl(SchedulerImpl& scheduler_, Runable& entry_)
    : scheduler(scheduler_)
    , entry(entry_)
    , stackStorage(fiberStackSize)
    , contextStorage(Context::implSize())
{
    context = Context::create(contextStorage.mutData(), stackStorage.mutData(), fiberStackSize, *this);
}

void FiberImpl::run() {
    entry.run();
    finished = true;
    // The final switch out; the resumer destroys the fiber afterwards, so
    // nothing may run on this stack again.
    context->switchTo(*resumeTo);
}

void FiberImpl::ready(PollFD) {
    fdReady = true;
    scheduler.resume(*this);
}

void FiberImpl::ready() {
    timerFired = true;
    scheduler.resume(*this);
}

void FiberImpl::block() {
    context->switchTo(*resumeTo);
}

SchedulerImpl::SchedulerImpl(Poller& poller_)
    : poller(poller_)
{
}

void SchedulerImpl::resume(FiberImpl& fiber) {
    // The host context lives on the resumer's stack frame, which stays alive
    // for as long as the fiber runs; nested resumes each bring their own.
    Context* const host = Context::create(alloca(Context::implSize()));
    FiberImpl* const previous = active;
    active = &fiber;
    fiber.resumeTo = host;
    host->switchTo(*fiber.context);
    active = previous;
    if (fiber.finished) {
        delete &fiber;
    }
}

void SchedulerImpl::spawn(Runable& entry) {
    resume(*(new FiberImpl(*this, entry)));
}

bool SchedulerImpl::awaitFd(int fd, u32 flags, u64 timeoutUs) {
    FiberImpl& fiber = *active;
    fiber.fdReady = false;
    fiber.timerFired = false;
    poller.arm(
        {
            .fd = fd,
            .flags = flags,
        },
        fiber
    );
    if (timeoutUs != 0) {
        poller.timeout(timeoutUs, fiber);
    }
    fiber.block();
    if (fiber.fdReady) {
        if (timeoutUs != 0) {
            poller.cancel(fiber);
        }
        return true;
    }
    poller.disarm(fd);
    return false;
}

bool SchedulerImpl::awaitReadable(int fd, u64 timeoutUs) {
    return awaitFd(fd, PollFlag::In, timeoutUs);
}

bool SchedulerImpl::awaitWritable(int fd, u64 timeoutUs) {
    return awaitFd(fd, PollFlag::Out, timeoutUs);
}

void SchedulerImpl::sleep(u64 timeoutUs) {
    FiberImpl& fiber = *active;
    fiber.timerFired = false;
    poller.timeout(timeoutUs, fiber);
    fiber.block();
}

void SchedulerImpl::yield() {
    sleep(0);
}

bool SchedulerImpl::inFiber() const {
    return active != nullptr;
}

Scheduler* Scheduler::create(ObjPool& owner, Poller& poller) {
    return owner.make<SchedulerImpl>(poller);
}
