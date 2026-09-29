#include "app.h"
#include "fiber.h"

#include <std/dbg/insist.h>

#include <stdio.h>
#include <unistd.h>

using namespace plt;
using namespace plt::e2e;
using namespace stl;

namespace {
    struct Terminal final: public App {
        Terminal();
        ~Terminal();
        void paint(Canvas& canvas) override;
        void text(const TextInput& input) override;
        int pipe[2];
        bool blocked = false;
    };
}

Terminal::Terminal() {
    STD_INSIST(::pipe(pipe) == 0);
}

Terminal::~Terminal() {
    ::close(pipe[0]);
    ::close(pipe[1]);
}

void Terminal::paint(Canvas& canvas) {
    canvas.clear(blocked ? 0xe0b040 : 0x204060);
    canvas.text(20, 40, StringView(u8"Terminal waiting for a remote response"));
}

void Terminal::text(const TextInput& input) {
    STD_INSIST(!blocked && input.codepoint == 'a');
    blocked = true;
    puts("INPUT BLOCKED");
    window->requestFrame();
    platform->scheduler()->awaitReadable(pipe[0], 0);
    STD_INSIST(false);
}

int main() {
    {
        Terminal app;
        app.open("plt-input-cancel");
        app.run();
    }
    puts("INPUT QUEUE RELEASED");
}
