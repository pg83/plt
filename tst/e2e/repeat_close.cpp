#include "app.h"

#include <stdio.h>
#include <string.h>

using namespace plt;
using namespace plt::e2e;
using namespace stl;

namespace {
    struct Document final: public App {
        Document(Platform* shared, bool preview);
        void paint(Canvas& canvas) override;
        void key(const KeyInput& input) override;
        void text(const TextInput& input) override;
        void flush() override;
        void retire();
        bool preview;
        bool typed = false;
        bool retireOnFlush = false;
    };
}

Document::Document(Platform* shared, bool preview_)
    : App(shared)
    , preview(preview_)
{
}

void Document::paint(Canvas& canvas) {
    canvas.clear(preview ? 0xe0b040 : typed ? 0x40a060 : 0x204060);
}

void Document::retire() {
    owner = ObjPool::fromMemory();
    window = nullptr;
    surface = nullptr;
    puts("PREVIEW RETIRED");
}

void Document::key(const KeyInput& input) {
    App::key(input);
    if (preview && input.action == InputAction::Repeat && strcmp(setting("PLT_RETIRE", "key"), "key") == 0) {
        retire();
    }
}

void Document::text(const TextInput& input) {
    if (preview) {
        const char* mode = setting("PLT_RETIRE", "key");
        if (strcmp(mode, "text") == 0) {
            retire();
        } else if (strcmp(mode, "flush") == 0) {
            retireOnFlush = true;
        }
    } else {
        printf("TEXT %u\n", input.codepoint);
        typed = true;
        window->requestFrame();
    }
}

void Document::flush() {
    if (retireOnFlush) {
        retireOnFlush = false;
        retire();
    }
}

int main() {
    auto owner = ObjPool::fromMemory();
    Platform* const platform = Platform::create(*owner);
    Document document(platform, false), preview(platform, true);
    document.open("plt-repeat-document");
    preview.open("plt-repeat-preview");
    document.run();
}
