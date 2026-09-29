#include "app.h"
#include "drop.h"

#include <std/ios/input.h>
#include <std/dbg/insist.h>
#include <std/lib/buffer.h>

#include <stdio.h>

using namespace plt;
using namespace plt::e2e;
using namespace stl;

namespace {
    struct Importer final: public App {
        void paint(Canvas& canvas) override;
        void key(const KeyInput& input) override;
        size_t imported = 0;
        size_t rejected = 0;
        bool complete = false;
    };
}

void Importer::key(const KeyInput& input) {
    App::key(input);
    if (input.action != InputAction::Press || input.baseCodepoint != 'v') {
        return;
    }
    Input* clipboard = window->secondary()->read();
    Buffer manifest;
    clipboard->readAll(manifest);
    delete clipboard;
    StringView remaining(manifest);
    StringView uri;
    FILE* output = fopen(setting("PLT_IMPORT_OUTPUT", ""), "wb");
    STD_INSIST(output != nullptr);
    while (nextUriListEntry(remaining, uri)) {
        Buffer path;
        if (!fileUriToPath(uri, path)) {
            ++rejected;
            continue;
        }
        FILE* source = fopen(path.cStr(), "rb");
        STD_INSIST(source != nullptr);
        char bytes[1024];
        size_t count;
        while ((count = fread(bytes, 1, sizeof(bytes), source)) != 0) {
            STD_INSIST(fwrite(bytes, 1, count, output) == count);
        }
        STD_INSIST(!ferror(source));
        STD_INSIST(fclose(source) == 0);
        ++imported;
    }
    STD_INSIST(remaining.empty());
    STD_INSIST(fclose(output) == 0);
    complete = true;
    printf("IMPORTED %zu REJECTED %zu\n", imported, rejected);
    window->requestFrame();
}

void Importer::paint(Canvas& canvas) {
    canvas.clear(complete ? 0x40a060 : 0x204060);
    canvas.text(20, 40, StringView(u8"Import local documents from a URI list"));
}

int main() {
    Importer app;
    app.open("plt-uri-import");
    app.run();
}
