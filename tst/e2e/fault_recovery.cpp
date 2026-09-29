#include "app.h"

#include <std/ios/input.h>
#include <std/ios/output.h>
#include <std/lib/buffer.h>

#include <stdio.h>

using namespace plt;
using namespace plt::e2e;
using namespace stl;

namespace {
    struct Editor final: public App {
        void paint(Canvas& canvas) override;
        void key(const KeyInput& input) override;
        u32 color = 0x204060;
    };
}

void Editor::paint(Canvas& canvas) {
    canvas.clear(color);
    canvas.text(20, 40, StringView(u8"Recoverable clipboard transfer"));
}

void Editor::key(const KeyInput& input) {
    App::key(input);
    if (input.action != InputAction::Press) {
        return;
    }
    if (input.baseCodepoint == 'v') {
        Input* stream = window->secondary()->read();
        Buffer content;
        stream->readAll(content);
        delete stream;
        printf("PASTE [%s]\n", content.cStr());
        color = content.empty() ? 0xc04040 : 0x40a060;
    } else if (input.baseCodepoint == 'c') {
        Output* stream = window->secondary()->write();
        stream->write("recovered", 9);
        stream->finish();
        delete stream;
        puts("COPY");
    }
    window->requestFrame();
}

int main() {
    Editor editor;
    editor.open("plt-fault-recovery");
    editor.run();
}
