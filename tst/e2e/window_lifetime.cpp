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
    struct Preview final: public App {
        explicit Preview(Platform* shared);
        void paint(Canvas& canvas) override;
    };

    struct Desktop final: public App, public PollCallback {
        explicit Desktop(Platform* shared);
        ~Desktop();
        void paint(Canvas& canvas) override;
        void key(const KeyInput& input) override;
        void ready(PollFD event) override;
        Preview* preview = nullptr;
        PollWaiter commands;
        int fd = -1;
        bool retired = false;
    };
}

Preview::Preview(Platform* shared)
    : App(shared)
{
}

void Preview::paint(Canvas& canvas) {
    canvas.clear(0xe0b040);
    canvas.text(20, 40, StringView(u8"Temporary preview"));
}

Desktop::Desktop(Platform* shared)
    : App(shared)
{
    fd = ::open(setting("PLT_COMMAND_PIPE", ""), O_RDWR | O_CLOEXEC);
    STD_INSIST(fd >= 0);
    commands.fd = {.fd = fd, .flags = PollFlag::In};
    commands.callback = this;
    platform->poller()->arm(commands);
}

Desktop::~Desktop() {
    platform->poller()->cancel(commands);
    ::close(fd);
}

void Desktop::paint(Canvas& canvas) {
    canvas.clear(retired ? 0x40a060 : 0x204060);
    canvas.text(20, 40, StringView(u8"Preview lifecycle"));
}

void Desktop::key(const KeyInput& input) {
    App::key(input);
    if (input.action == InputAction::Press && input.baseCodepoint == 'p') {
        puts("DISPATCH PAUSED");
        char command;
        STD_INSIST(read(fd, &command, 1) == 1 && command == 'r');
    }
}

void Desktop::ready(PollFD) {
    char command;
    STD_INSIST(read(fd, &command, 1) == 1 && command == '!');
    auto* const display = static_cast<wl_display*>(window->renderContext().connection);
    auto* const queue = wl_display_create_queue(display);
    // Like a renderer waiting on its own WSI queue, read all preceding
    // events without delivering the default queue. The preview is retired
    // before those already-received input events are dispatched.
    STD_INSIST(wl_display_roundtrip_queue(display, queue) >= 0);
    wl_event_queue_destroy(queue);
    preview->window->requestFocus();
    preview->owner = ObjPool::fromMemory();
    preview->window = nullptr;
    preview->surface = nullptr;
    retired = true;
    puts("PREVIEW RETIRED");
    window->requestFrame();
}

int main() {
    auto owner = ObjPool::fromMemory();
    Platform* const platform = Platform::create(*owner);
    Desktop desktop(platform);
    Preview preview(platform);
    desktop.preview = &preview;
    desktop.open("plt-lifetime-desktop");
    preview.open("plt-lifetime-preview");
    desktop.run();
}
