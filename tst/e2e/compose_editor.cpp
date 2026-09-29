#include "app.h"

#include <stdio.h>
#include <string.h>

using namespace plt;
using namespace plt::e2e;
using namespace stl;

namespace {
    struct Editor final: public App {
        explicit Editor(Platform* shared);
        void paint(Canvas& canvas) override;
        void text(const TextInput& input) override;
        void preedit(StringView text, i32 begin, i32 end) override;
        unsigned count = 0;
        bool composing = false;
        bool closeOnText = false;
    };
}

Editor::Editor(Platform* shared)
    : App(shared)
{
}

void Editor::paint(Canvas& canvas) {
    canvas.clear(composing ? 0xe0b040 : count ? 0x40a060 : 0x204060);
    canvas.text(20, 40, StringView(u8"Multilingual editor"));
}

void Editor::text(const TextInput& input) {
    printf("TEXT %u %u\n", ++count, input.codepoint);
    if (closeOnText) {
        owner = ObjPool::fromMemory();
        window = nullptr;
        surface = nullptr;
        puts("EDITOR CLOSED");
        return;
    }
    window->requestFrame();
}

void Editor::preedit(StringView text, i32 begin, i32 end) {
    composing = !text.empty();
    printf("PREEDIT %zu %d %d\n", text.length(), begin, end);
    window->requestFrame();
}

int main() {
    auto owner = ObjPool::fromMemory();
    Platform* const platform = Platform::create(*owner);
    Editor editor(platform), survivor(platform);
    editor.closeOnText = strcmp(setting("PLT_CLOSE_ON_TEXT", "0"), "1") == 0;
    survivor.passive = strcmp(setting("PLT_EXTRA_PASSIVE", "0"), "1") == 0;
    if (editor.closeOnText || survivor.passive) {
        survivor.open("plt-compose-survivor");
    }
    editor.open("plt-compose-editor");
    editor.window->requestTextInputRect(24, 50, 12, strcmp(setting("PLT_ZERO_CARET", "0"), "1") == 0 ? 0 : 20);
    editor.run();
}
