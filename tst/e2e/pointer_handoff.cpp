#include "app.h"

#include <std/dbg/insist.h>

#include <stdio.h>

using namespace plt;
using namespace plt::e2e;
using namespace stl;

namespace {
    struct Document final: public App {
        Document(Platform* shared, bool source, bool& waiting);
        void paint(Canvas& canvas) override;
        void pointerPresence(bool present) override;
        void pointerButton(const PointerButtonInput& input) override;
        bool source;
        bool& waiting;
        bool entered = false;
        bool clicked = false;
    };
}

Document::Document(Platform* shared, bool source_, bool& waiting_)
    : App(shared)
    , source(source_)
    , waiting(waiting_)
{
}

void Document::paint(Canvas& canvas) {
    canvas.clear(clicked ? 0x40a060 : source ? 0xe0b040 : 0x204060);
    canvas.text(20, 40, StringView(u8"Modal pointer handoff"));
}

void Document::pointerPresence(bool present) {
    printf("PRESENCE %d %d\n", source, present);
    if (source && !present && entered) {
        entered = false;
        waiting = true;
        platform->run();
        STD_INSIST(!waiting);
        puts("HANDOFF COMPLETE");
    } else if (!source && present && waiting) {
        waiting = false;
        platform->stop();
    } else if (source && present) {
        entered = true;
    }
}

void Document::pointerButton(const PointerButtonInput& input) {
    if (!source && input.pressed) {
        clicked = true;
        puts("DESTINATION CLICKED");
        window->requestFrame();
    }
}

int main() {
    auto owner = ObjPool::fromMemory();
    Platform* const platform = Platform::create(*owner);
    bool waiting = false;
    Document source(platform, true, waiting), target(platform, false, waiting);
    source.open("plt-handoff-source");
    target.open("plt-handoff-target");
    target.run();
}
