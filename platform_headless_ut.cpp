#include "platform_headless.h"

#include <std/tst/ut.h>
#include <std/mem/obj_pool.h>

#include <string.h>

using namespace plt;
using namespace stl;

namespace {
    struct FrameProbe final: FrameCallback {
        bool frame(const WindowInfo& info) override;

        WindowHeadless* window = nullptr;
        WindowInfo seen;
        u32 resizeWidth = 0;
        u32 resizeHeight = 0;
        u8 fill = 0;
        size_t calls = 0;
        bool targetReady = false;
    };
}

bool FrameProbe::frame(const WindowInfo& info) {
    ++calls;
    seen = info;
    const RenderContext context = window->renderContext();
    auto* const target = static_cast<HeadlessRenderTarget*>(context.window);
    targetReady = target != nullptr && target->pixels != nullptr;
    if (targetReady) {
        memset(target->pixels, fill, target->length);
    }
    if (resizeWidth != 0 && resizeHeight != 0) {
        const u32 width = resizeWidth;
        const u32 height = resizeHeight;
        resizeWidth = 0;
        resizeHeight = 0;
        window->requestResize(width, height);
    }
    return targetReady;
}

STD_TEST_SUITE(PlatformHeadless) {
    STD_TEST(FrameRequestsAreDeferredAndFrameInfoIsStable) {
        auto pool = ObjPool::fromMemory();
        Platform* const platform = createHeadlessPlatform(*pool);
        FrameProbe probe;
        auto& window = static_cast<WindowHeadless&>(*platform->createWindow(
            *pool,
            {
                .width = 3,
                .height = 2,
                .frame = &probe,
            }
        ));
        probe.window = &window;
        probe.resizeWidth = 5;
        probe.resizeHeight = 4;
        probe.fill = 0x5a;

        window.requestFrame();
        window.requestFrame();

        STD_INSIST(window.framePending());
        STD_INSIST(probe.calls == 0);
        STD_INSIST(window.dispatchFrame());
        STD_INSIST(probe.calls == 1);
        STD_INSIST(probe.seen.width == 3);
        STD_INSIST(probe.seen.height == 2);
        STD_INSIST(window.info().width == 5);
        STD_INSIST(window.info().height == 4);
        STD_INSIST(window.framePending());
        const HeadlessFrame first = window.presentedFrame();
        STD_INSIST(first.width == 3);
        STD_INSIST(first.height == 2);
        STD_INSIST(first.length == 18);
        STD_INSIST(first.pixels[0] == 0x5a);

        STD_INSIST(window.dispatchFrame());
        STD_INSIST(probe.calls == 2);
        STD_INSIST(probe.seen.width == 5);
        STD_INSIST(probe.seen.height == 4);
        STD_INSIST(!window.framePending());
    }

    STD_TEST(FailedPresentationPreservesFrontBuffer) {
        auto pool = ObjPool::fromMemory();
        Platform* const platform = createHeadlessPlatform(*pool);
        FrameProbe probe;
        auto& window = static_cast<WindowHeadless&>(*platform->createWindow(
            *pool,
            {
                .width = 2,
                .height = 2,
                .frame = &probe,
            }
        ));
        probe.window = &window;
        probe.fill = 0x11;
        window.requestFrame();
        STD_INSIST(window.dispatchFrame());
        const HeadlessFrame first = window.presentedFrame();
        STD_INSIST(first.generation == 1);
        STD_INSIST(first.pixels[0] == 0x11);

        probe.fill = 0x22;
        window.failNextPresentation();
        STD_INSIST(!window.dispatchFrame());
        const HeadlessFrame failed = window.presentedFrame();
        STD_INSIST(failed.generation == 1);
        STD_INSIST(failed.pixels[0] == 0x11);

        window.requestFrame();
        STD_INSIST(window.dispatchFrame());
        const HeadlessFrame second = window.presentedFrame();
        STD_INSIST(second.generation == 2);
        STD_INSIST(second.pixels[0] == 0x22);
    }
}
