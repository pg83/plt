#include "app.h"

#include <stdio.h>

using namespace plt;
using namespace plt::e2e;
using namespace stl;

namespace {
    struct Editor final: public App {
        void paint(Canvas& canvas) override;
        void text(const TextInput& input) override;
        void preedit(StringView text, i32 begin, i32 end) override;
        unsigned count = 0;
        bool composing = false;
    };
}

void Editor::paint(Canvas& canvas) {
    canvas.clear(composing ? 0xe0b040 : count ? 0x40a060 : 0x204060);
    canvas.text(20, 40, StringView(u8"Multilingual editor"));
}

void Editor::text(const TextInput& input) {
    printf("TEXT %u %u\n", ++count, input.codepoint);
    window->requestFrame();
}

void Editor::preedit(StringView text, i32 begin, i32 end) {
    composing = !text.empty();
    printf("PREEDIT %zu %d %d\n", text.length(), begin, end);
    window->requestFrame();
}

int main() {
    Editor editor;
    editor.open("plt-compose-editor");
    editor.window->requestTextInputRect(24, 50, 12, 20);
    editor.run();
}
