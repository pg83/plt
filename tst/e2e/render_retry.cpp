#include "app.h"

#include <stdio.h>

using namespace plt;
using namespace plt::e2e;
using namespace stl;

namespace {
    struct Retry final: public App {
        bool frame(const WindowInfo& info) override;
        void paint(Canvas& canvas) override;
        int failures = 3;
        bool requested = false;
    };
}

bool Retry::frame(const WindowInfo& info) {
    if (failures > 0) {
        printf("RETRY %d\n", failures--);
        window->requestFrame();
        return false;
    }
    const bool rendered = App::frame(info);
    if (rendered && !requested) {
        requested = true;
        window->requestFrame();
        puts("REPAINT DURING PRESENT");
    }
    return rendered;
}

void Retry::paint(Canvas& c) {
    c.clear(0x40a060);
    c.text(20, 40, StringView(u8"Presentation recovered"));
}

int main() {
    Retry app;
    app.open("plt-retry");
    app.run();
}
