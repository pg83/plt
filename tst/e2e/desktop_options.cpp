#include "app.h"
#include "fiber.h"

#include <std/ios/input.h>
#include <std/dbg/insist.h>
#include <std/ios/output.h>
#include <std/lib/buffer.h>
#include <std/thr/runable.h>

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

using namespace plt;
using namespace plt::e2e;
using namespace stl;

namespace {
    struct Preview final: public App, public Runable {
        ~Preview();
        void run() override;
        void paint(Canvas& canvas) override;
        void key(const KeyInput& input) override;
        int commands = -1;
        u32 color = 0x204060;
    };
}

Preview::~Preview() {
    if (commands >= 0) {
        ::close(commands);
    }
}

void Preview::paint(Canvas& canvas) {
    canvas.clear(color);
    canvas.text(20, 40, StringView(u8"Desktop capability fallback"));
}

void Preview::key(const KeyInput& input) {
    printf("KEY %u %u\n", input.baseCodepoint, input.modifiers);
    App::key(input);
}

void Preview::run() {
    for (;;) {
        STD_INSIST(platform->scheduler()->awaitReadable(commands, 0));
        char cmd;
        if (read(commands, &cmd, 1) != 1) {
            continue;
        }
        if (cmd == 'q') {
            window->requestClose();
            return;
        }
        if (cmd == 'c') {
            Clipboard* clipboards[] = {window->primary(), window->secondary()};
            for (Clipboard* clipboard : clipboards) {
                Output* output = clipboard->write();
                output->write("preview selection", 17);
                output->finish();
                output->finish();
                delete output;
                Input* input = clipboard->read();
                Buffer content;
                char first;
                if (input->read(&first, 1) != 0) {
                    content.append(&first, 1);
                }
                input->readAll(content);
                delete input;
                printf("SELECTION [%s]\n", content.cStr());
            }
        } else if (cmd == 'w') {
            window->requestMove(80, 100);
            window->requestAttention();
            window->requestFocus();
            window->requestFocus();
            window->requestMinimumSize(100, 100);
            window->requestResizeUnit(1, 1, 0, 0);
            window->requestResize(400, 280);
            window->requestTextInputRect(0, 0, 8, 16);
            window->requestTextInputRect(0, 0, 8, 16);
            window->requestTextInputRect(1, 0, 8, 16);
            window->requestTextInputRect(1, 1, 8, 16);
            window->requestTextInputRect(1, 1, 9, 16);
            window->requestTextInputRect(1, 1, 9, 17);
            window->requestPointerIcon(static_cast<PointerIcon>(255));
            for (unsigned icon = 0; icon != 37; ++icon) {
                window->requestPointerIcon((PointerIcon)icon);
            }
        }
        color = 0x40a060;
        window->requestFrame();
        printf("COMMAND %c\n", cmd);
    }
}

int main() {
    Preview app;
    app.open("plt-desktop-options");
    app.commands = ::open(setting("PLT_COMMAND_PIPE", ""), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    STD_INSIST(app.commands >= 0);
    Clipboard* clipboards[] = {app.window->primary(), app.window->secondary()};
    for (Clipboard* clipboard : clipboards) {
        Output* output = clipboard->write();
        output->write("preview selection", 17);
        output->finish();
        delete output;
    }
    app.window->requestMove(80, 100);
    app.window->requestFocus();
    app.window->requestTextInputRect(0, 0, 8, 0);
    app.platform->scheduler()->create(*app.owner, app, 128 * 1024);
    app.App::run();
}
