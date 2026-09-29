#include "app.h"

#include <std/sys/throw.h>

#include <stdio.h>

using namespace plt;
using namespace plt::e2e;
using namespace stl;

namespace {
    struct Viewer final: public App {
        void paint(Canvas& canvas) override;
    };
}

void Viewer::paint(Canvas& canvas) {
    canvas.clear(0x40a060);
    canvas.text(20, 40, StringView(u8"Desktop connection recovered"));
}

int main() {
    try {
        Viewer viewer;
        viewer.open("plt-startup");
        viewer.run();
    } catch (Exception& error) {
        const StringView description = error.description();
        fprintf(stderr, "STARTUP ERROR %.*s\n", (int)description.length(), description.data());
        return 2;
    }
}
