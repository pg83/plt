#include "app.h"
#include "poller.h"

#include <std/dbg/insist.h>
#include <std/thr/poll_fd.h>

#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <wayland-client.h>

using namespace plt;
using namespace plt::e2e;
using namespace stl;

namespace {
    struct Desktop final: public App, public PollCallback {
        Desktop();
        ~Desktop();
        void paint(Canvas& canvas) override;
        void key(const KeyInput& input) override;
        void ready(PollFD event) override;
        static void done(void* data, wl_callback* callback, u32 serial);
        PollWaiter commands;
        wl_callback* pending = nullptr;
        int fd;
    };
}

Desktop::Desktop() {
    fd = ::open(setting("PLT_COMMAND_PIPE", ""), O_RDWR | O_CLOEXEC);
    STD_INSIST(fd >= 0);
    commands.fd = {.fd = fd, .flags = PollFlag::In};
    commands.callback = this;
    platform->poller()->arm(commands);
}

Desktop::~Desktop() {
    platform->poller()->cancel(commands);
    if (pending != nullptr) {
        wl_callback_destroy(pending);
    }
    ::close(fd);
}

void Desktop::paint(Canvas& canvas) {
    canvas.clear(0x40a060);
}

void Desktop::key(const KeyInput& input) {
    App::key(input);
    if (input.action == InputAction::Press && input.baseCodepoint == 'p') {
        puts("DISPATCH PAUSED");
        char command;
        STD_INSIST(read(fd, &command, 1) == 1 && command == 'r');
    }
}

void Desktop::done(void* data, wl_callback* callback, u32) {
    auto& app = *static_cast<Desktop*>(data);
    STD_INSIST(callback == app.pending);
    app.pending = nullptr;
    wl_callback_destroy(callback);
    puts("BACKLOG DISPATCHED");
}

void Desktop::ready(PollFD) {
    char command;
    STD_INSIST(read(fd, &command, 1) == 1 && command == '!');
    auto* const display = static_cast<wl_display*>(window->renderContext().connection);
    pending = wl_display_sync(display);
    static const wl_callback_listener listener{done};
    wl_callback_add_listener(pending, &listener, this);
    auto* const queue = wl_display_create_queue(display);
    STD_INSIST(wl_display_roundtrip_queue(display, queue) >= 0);
    wl_event_queue_destroy(queue);
    puts("FOREIGN QUEUE DONE");
    // No new plt request here: the display waiter from this same poll round
    // must drain the default events received on behalf of the foreign queue.
}

int main() {
    Desktop app;
    app.open("plt-backlog");
    app.run();
}
