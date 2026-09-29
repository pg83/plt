#include "app.h"
#include "poller.h"
#include "platform_headless.h"

#include <std/dbg/insist.h>

#include <stdio.h>

using namespace plt;
using namespace plt::e2e;
using namespace stl;

namespace {
    struct Dialog final: public TimerCallback {
        explicit Dialog(Platform& platform);
        void ready() override;
        Platform& platform;
        bool returned = false;
    };

    struct Completion final: public TimerCallback {
        Completion(Platform& platform, bool stop);
        void ready() override;
        Platform& platform;
        bool stop;
        bool delivered = false;
    };

    struct Preview final: public App {
        void paint(Canvas& canvas) override;
    };
}

Dialog::Dialog(Platform& platform_)
    : platform(platform_)
{
}

void Dialog::ready() {
    // An embedded preview synchronously waits for export completion while
    // continuing to service the other timers of its headless event loop.
    platform.run();
    returned = true;
    platform.stop();
}

Completion::Completion(Platform& platform_, bool stop_)
    : platform(platform_)
    , stop(stop_)
{
}

void Completion::ready() {
    STD_INSIST(!delivered);
    delivered = true;
    if (stop) {
        platform.stop();
    }
}

void Preview::paint(Canvas& canvas) {
    canvas.clear(0x40a060);
    canvas.text(20, 40, StringView(u8"Export finished during the modal wait"));
}

int main() {
    Preview app;
    auto owner = ObjPool::fromMemory();
    Platform* const exporter = createHeadlessPlatform(*owner);
    Dialog dialog(*exporter);
    Completion progress(*exporter, false), finish(*exporter, true);
    exporter->poller()->deadline(1, dialog);
    exporter->poller()->deadline(1, progress);
    exporter->poller()->timeout(1000, finish);
    exporter->run();
    STD_INSIST(dialog.returned && progress.delivered && finish.delivered);
    puts("MODAL EXPORT COMPLETE");
    app.open("plt-timer-dialog");
    app.run();
}
