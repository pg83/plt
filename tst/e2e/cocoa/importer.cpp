// Windowless plt application: imports a document delivered by a worker,
// with a periodic progress timer and a cross-thread completion notification.
#include "fiber.h"
#include "platform.h"
#include "poller.h"
#include "loop_wake.h"

#include <std/dbg/insist.h>
#include <std/sys/crt.h>
#include <std/mem/obj_pool.h>
#include <std/thr/runable.h>
#include <std/thr/poll_fd.h>

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace plt;
using namespace stl;

namespace {
    struct Importer final: public Runable, public TimerCallback {
        Importer();
        void run() override;
        void ready() override;
        static void* produce(void* self);
        ObjPool::Ref owner = ObjPool::fromMemory();
        Platform* platform = Platform::create(*owner);
        LoopWake* wake;
        int sockets[2];
        bool imported = false;
        bool notified = false;
        unsigned long long checksum = 0;
        size_t total = 0;
    };

    // A speculative import is cancelled when a newer document replaces it.
    // Its pending OS readiness/deadline must never resume the freed task.
    struct Preview final: public Runable {
        Preview(Importer& app, u64 timeout);
        void run() override;
        Importer& app;
        u64 timeout;
    };

    // A paused indexing task resumes through its persistent application handle.
    struct Indexer final: public Runable, public TimerCallback {
        explicit Indexer(Scheduler& scheduler);
        void run() override;
        void ready() override;
        Scheduler& scheduler;
        Fiber* handle = nullptr;
        bool done = false;
    };

    struct Report final: public TimerCallback {
        explicit Report(const char* label);
        void ready() override;
        const char* label;
        Poller* poller = nullptr;
        Report* superseded = nullptr;
        bool delivered = false;
    };

    struct RetiredSubscription final: public PollCallback {
        void ready(PollFD event) override;
    };

    struct Progress final: public TimerCallback {
        explicit Progress(Importer& app);
        void ready() override;
        Importer& app;
        unsigned ticks = 0;
    };
}

Importer::Importer() {
    STD_INSIST(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    STD_INSIST(fcntl(sockets[0], F_SETFL, O_NONBLOCK) == 0);
    wake = platform->createLoopWake(*owner, *this);
    // stop before run must be consumed once, including without NSApplication.
    platform->stop();
    platform->run();
}

void* Importer::produce(void* self) {
    Importer& app = *(Importer*)self;
    FILE* input = fopen(getenv("PLT_IMPORT_FILE"), "rb");
    STD_INSIST(input != nullptr);
    char greeting[5];
    STD_INSIST(read(app.sockets[1], greeting, sizeof(greeting)) == sizeof(greeting));
    STD_INSIST(memcmp(greeting, "READY", sizeof(greeting)) == 0);
    unsigned char chunk[4096];
    size_t count;
    // Let the initial read time out, then deliver a real document in pieces.
    usleep(30'000);
    while ((count = fread(chunk, 1, sizeof(chunk), input)) != 0) {
        size_t offset = 0;
        while (offset != count) {
            ssize_t n = write(app.sockets[1], chunk + offset, count - offset);
            STD_INSIST(n > 0);
            offset += n;
        }
        usleep(1000);
    }
    fclose(input);
    close(app.sockets[1]);
    app.wake->signal();
    return nullptr;
}

void Importer::run() {
    Scheduler* scheduler = platform->scheduler();
    STD_INSIST(scheduler->awaitWritable(sockets[0], 1'000'000));
    STD_INSIST(write(sockets[0], "READY", 5) == 5);
    STD_INSIST(!scheduler->awaitReadable(sockets[0], 1000));
    puts("INITIAL TIMEOUT");
    for (;;) {
        STD_INSIST(scheduler->awaitReadable(sockets[0], 1'000'000));
        unsigned char chunk[1024];
        ssize_t count = read(sockets[0], chunk, sizeof(chunk));
        STD_INSIST(count >= 0);
        if (count == 0) {
            break;
        }
        for (ssize_t i = 0; i != count; ++i) {
            checksum = checksum * 131 + chunk[i];
        }
        total += count;
        scheduler->yield();
    }
    close(sockets[0]);
    imported = true;
    printf("IMPORTED %zu %llu\n", total, checksum);
    if (notified) {
        platform->stop();
    }
}

void Importer::ready() {
    notified = true;
    puts("WORKER DONE");
    if (imported) {
        platform->stop();
    }
}

Preview::Preview(Importer& app_, u64 timeout_)
    : app(app_)
    , timeout(timeout_)
{
}

void Preview::run() {
    app.platform->scheduler()->awaitReadable(app.sockets[0], timeout);
    // This request was replaced before the worker produced any bytes.
    STD_INSIST(false);
}

Indexer::Indexer(Scheduler& scheduler_)
    : scheduler(scheduler_)
{
}

void Indexer::run() {
    scheduler.current()->park();
    handle->wake();
    STD_INSIST(handle->parkFor(100'000));
    handle->wake();
    handle->park();
    STD_INSIST(!handle->parkFor(1000));
    done = true;
    puts("INDEXED");
}

void Indexer::ready() {
    handle->wake();
}

Report::Report(const char* label_)
    : label(label_)
{
}

void Report::ready() {
    delivered = true;
    printf("REPORT %s\n", label);
    if (superseded != nullptr) {
        poller->cancel(*superseded);
    }
}

Progress::Progress(Importer& app_)
    : app(app_)
{
}

void Progress::ready() {
    ++ticks;
    printf("PROGRESS %zu\n", app.total);
    app.platform->poller()->deadline(monotonicNowUs() + 2000, *this);
}

void RetiredSubscription::ready(PollFD) {
    STD_INSIST(false);
}

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    Importer app;
    int retired[2];
    STD_INSIST(pipe(retired) == 0);
    RetiredSubscription subscription;
    PollWaiter waiter;
    waiter.fd = {.fd = retired[0], .flags = PollFlag::In};
    waiter.callback = &subscription;
    app.platform->poller()->arm(waiter);
    app.platform->poller()->cancel(waiter);
    STD_INSIST(write(retired[1], "x", 1) == 1);
    Progress progress(app);
    app.platform->poller()->deadline(0, progress);
    Report stale("stale"), current("current"), duplicate("duplicate");
    current.poller = app.platform->poller();
    current.superseded = &duplicate;
    // Coalesce obsolete status updates, including one already queued for
    // this event-loop pass when the replacement is delivered.
    app.platform->poller()->defer(stale);
    app.platform->poller()->defer(current);
    app.platform->poller()->defer(duplicate);
    app.platform->poller()->cancel(stale);
    Preview obsolete(app, 0);
    Preview expired(app, 2000);
    Preview replaced(app, 1'000'000);
    {
        ObjPool::Ref requests = ObjPool::fromMemory();
        app.platform->scheduler()->create(*requests, obsolete);
        app.platform->scheduler()->create(*requests, expired);
        app.platform->scheduler()->create(*requests, replaced);
    }
    Indexer indexer(*app.platform->scheduler());
    indexer.handle = app.platform->scheduler()->create(*app.owner, indexer);
    app.platform->poller()->defer(indexer);
    pthread_t thread;
    STD_INSIST(pthread_create(&thread, nullptr, Importer::produce, &app) == 0);
    app.platform->scheduler()->create(*app.owner, app, 128 * 1024);
    app.platform->run();
    app.platform->poller()->cancel(progress);
    STD_INSIST(indexer.done);
    indexer.handle->wake();
    STD_INSIST(pthread_join(thread, nullptr) == 0);
    char unread;
    STD_INSIST(read(retired[0], &unread, 1) == 1 && unread == 'x');
    close(retired[0]);
    close(retired[1]);
    STD_INSIST(app.imported && app.notified && progress.ticks > 0);
    STD_INSIST(current.delivered && !stale.delivered && !duplicate.delivered);
}
