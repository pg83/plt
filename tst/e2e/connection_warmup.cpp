#include "app.h"
#include "poller.h"

#include <std/dbg/insist.h>

#include <stdio.h>
#include <wayland-client.h>

using namespace plt;
using namespace plt::e2e;
using namespace stl;

namespace {
    struct Document final: public App {
        explicit Document(Platform* platform);
        void paint(Canvas& canvas) override;
    };

    struct Warmup final: public TimerCallback {
        explicit Warmup(Platform& platform);
        void ready() override;
        Platform& platform;
    };
}

Document::Document(Platform* platform)
    : App(platform)
{
}

void Document::paint(Canvas& canvas) {
    canvas.clear(0x204060);
}

Warmup::Warmup(Platform& platform_)
    : platform(platform_)
{
}

void Warmup::ready() {
    platform.stop();
}

int main() {
    auto owner = ObjPool::fromMemory();
    Platform* const platform = Platform::create(*owner);
    Document document(platform);
    {
        // Prepare the display before opening the document. Drain registry
        // replies so retrying an unwritable connection cannot piggyback on
        // unrelated input readiness.
        auto preview = ObjPool::fromMemory();
        Window* const window = platform->createWindow(*preview, {});
        auto* const display = static_cast<wl_display*>(window->renderContext().connection);
        STD_INSIST(wl_display_roundtrip(display) >= 0);
        STD_INSIST(wl_display_roundtrip(display) >= 0);
        Warmup warmup(*platform);
        platform->poller()->timeout(20'000, warmup);
        platform->run();
        puts("CONNECTION WARMED UP");
    }
    document.open("plt-connection-warmup");
    document.run();
}
