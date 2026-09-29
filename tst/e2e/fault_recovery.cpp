#include "app.h"

#include <std/ios/input.h>
#include <std/dbg/insist.h>
#include <std/dbg/insist.h>
#include <std/ios/output.h>
#include <std/lib/buffer.h>

#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

using namespace plt;
using namespace plt::e2e;
using namespace stl;

namespace {
    struct Editor final: public App {
        void paint(Canvas& canvas) override;
        void key(const KeyInput& input) override;
        bool readInFrame = false;
        u32 color = 0x204060;
    };
}

void Editor::paint(Canvas& canvas) {
    if (readInFrame) {
        readInFrame = false;
        Input* stream = window->secondary()->read();
        char byte;
        STD_INSIST(stream->read(&byte, 1) == 0);
        delete stream;
        puts("PREVIEW UNAVAILABLE");
    }
    canvas.clear(color);
    canvas.text(20, 40, StringView(u8"Recoverable clipboard transfer"));
}

void Editor::key(const KeyInput& input) {
    App::key(input);
    if (input.action != InputAction::Press) {
        return;
    }
    if (input.baseCodepoint == 'g') {
        readInFrame = true;
    } else if (input.baseCodepoint == 'v') {
        Input* stream = window->secondary()->read();
        Buffer content;
        stream->readAll(content);
        delete stream;
        printf("PASTE [%s]\n", content.cStr());
        color = content.empty() ? 0xc04040 : 0x40a060;
    } else if (input.baseCodepoint == 'c') {
        Output* stream = window->secondary()->write();
        if (getenv("PLT_LARGE_COPY") != nullptr) {
            char block[4096];
            for (char& byte : block) {
                byte = 'x';
            }
            for (unsigned i = 0; i != 1024; ++i) {
                stream->write(block, sizeof(block));
            }
        } else {
            stream->write("recovered", 9);
        }
        stream->finish();
        delete stream;
        puts("COPY");
    }
    window->requestFrame();
}

int main() {
    sigset_t blocked;
    sigset_t previous;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGPIPE);
    const bool pending = strcmp(setting("PLT_PENDING_SIGPIPE", "0"), "1") == 0;
    if (strcmp(setting("PLT_IGNORE_SIGPIPE", "0"), "1") == 0) {
        signal(SIGPIPE, SIG_IGN);
    }
    if (pending) {
        STD_INSIST(pthread_sigmask(SIG_BLOCK, &blocked, &previous) == 0);
        STD_INSIST(raise(SIGPIPE) == 0);
    }
    Editor editor;
    editor.open("plt-fault-recovery");
    editor.run();
    if (pending) {
        sigset_t signals;
        STD_INSIST(sigpending(&signals) == 0 && sigismember(&signals, SIGPIPE) == 1);
        const timespec timeout{};
        STD_INSIST(sigtimedwait(&blocked, nullptr, &timeout) == SIGPIPE);
        STD_INSIST(pthread_sigmask(SIG_SETMASK, &previous, nullptr) == 0);
        puts("PENDING SIGPIPE PRESERVED");
    }
}
