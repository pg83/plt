#include "app.h"

#include <stdio.h>

using namespace plt;
using namespace plt::e2e;
using namespace stl;

namespace {
    struct Keymap final: public App {
        void paint(Canvas& canvas) override;
        void key(const KeyInput& input) override;
        void text(const TextInput& input) override;
        bool active = false;
    };
}

void Keymap::paint(Canvas& canvas) {
    canvas.clear(active ? 0x40a060 : 0x204060);
    canvas.text(20, 40, StringView(u8"Keyboard shortcut inspector"));
}

void Keymap::key(const KeyInput& input) {
    App::key(input);
    printf("KEY %u %u %u %u\n", (unsigned)input.key, (unsigned)input.action, input.modifiers, input.layoutCodepoint);
    active = true;
    window->requestFrame();
}

void Keymap::text(const TextInput& input) {
    printf("TEXT %u\n", input.codepoint);
}

int main() {
    Keymap viewer;
    viewer.open("plt-keymap-viewer");
    viewer.run();
}
