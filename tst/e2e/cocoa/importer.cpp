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

#include <fcntl.h>
#include <stdio.h>
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

Progress::Progress(Importer& app_)
    : app(app_)
{
}

void Progress::ready() {
    ++ticks;
    printf("PROGRESS %zu\n", app.total);
    app.platform->poller()->deadline(monotonicNowUs() + 2000, *this);
}

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    Importer app;
    Progress progress(app);
    app.platform->poller()->timeout(1000, progress);
    Preview obsolete(app, 0);
    Preview expired(app, 2000);
    {
        ObjPool::Ref requests = ObjPool::fromMemory();
        app.platform->scheduler()->create(*requests, obsolete);
        app.platform->scheduler()->create(*requests, expired);
    }
    pthread_t thread;
    STD_INSIST(pthread_create(&thread, nullptr, Importer::produce, &app) == 0);
    app.platform->scheduler()->create(*app.owner, app, 128 * 1024);
    app.platform->run();
    app.platform->poller()->cancel(progress);
    STD_INSIST(pthread_join(thread, nullptr) == 0);
    STD_INSIST(app.imported && app.notified && progress.ticks > 0);
}
