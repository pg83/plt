#include "test.h"

#include <stdio.h>

namespace plt::test {
    bool windowApi(int fd) {
        EventSink events;
        Client client(fd, 800, 1, &events, nullptr, true, &events);
        client.window->show();

        const RenderContext render = client.window->renderContext();
        const WindowInfo initial = events.lastInfo;
        if (render.backend != RenderBackend::Wayland
            || render.connection == nullptr || render.window == nullptr
            || initial.width != 800 || initial.height != 600
            || initial.screenPixelWidth != 1920
            || initial.screenPixelHeight != 1080
            || initial.contentScale != 1
            || initial.focused || initial.maximized || initial.fullscreen
            || initial.tiled || initial.iconified
            || events.frameCount == 0
            || events.lastInfo.width != initial.width
            || events.lastInfo.height != initial.height) {
            fprintf(stderr, "window API: invalid initial state or render context\n");
            return false;
        }

        const u32 frames = events.frameCount;
        client.window->invalidate();
        client.window->requestClose();
        client.window->requestClose();
        pump(*client.platform);
        if (events.frameCount != frames + 1 || events.closeCount != 1) {
            fprintf(stderr, "window API: direct callbacks were not idempotent\n");
            return false;
        }

        client.window->setTitle(stl::StringView(u8"updated title"));
        client.window->setMinimumSize(320, 240);
        client.window->setResizeUnit(10, 20, 3, 7);
        command(fd, Command::PointerEnter);
        pump(*client.platform);
        client.window->move(11, 22);
        client.window->setMaximized(true);
        client.window->setMaximized(false);
        client.window->setFullscreen(true);
        client.window->setFullscreen(false);
        client.window->restore();
        client.window->iconify();
        client.window->requestAttention();
        pump(*client.platform);
        if (command(fd, Command::QueryActivation).count != 1) {
            fprintf(stderr, "window API: requestAttention did not activate\n");
            return false;
        }
        client.window->focus();
        pump(*client.platform);
        if (command(fd, Command::QueryActivation).count != 2) {
            fprintf(stderr, "window API: focus did not activate\n");
            return false;
        }

        const u32 expectedRequests =
            UpdatedTitle | InitialAppId | Move | Maximize | Unmaximize
            | Fullscreen | Unfullscreen | Minimize;
        const Reply requests = command(fd, Command::QueryWindowRequests);
        const Reply minimum = command(fd, Command::QueryMinimum);
        if ((requests.count & expectedRequests) != expectedRequests
            || minimum.first != 320 || minimum.second != 240) {
            fprintf(
                stderr,
                "window API: request mask=%x minimum=%dx%d\n",
                requests.count,
                minimum.first,
                minimum.second
            );
            return false;
        }

        command(fd, Command::ConfigureWindowState);
        pump(*client.platform);
        const WindowInfo state = events.lastInfo;
        if (state.width != 900 || state.height != 700 || !state.focused
            || !state.maximized || !state.fullscreen || !state.tiled) {
            fprintf(stderr, "window API: configured state was not exposed\n");
            return false;
        }

        command(fd, Command::ConfigureWindowResize);
        pump(*client.platform);
        const WindowInfo resized = events.lastInfo;
        if (resized.width != 813 || resized.height != 627
            || resized.focused || resized.maximized
            || resized.fullscreen || resized.tiled) {
            fprintf(
                stderr,
                "window API: snapped size=%ux%u expected 813x627\n",
                resized.width,
                resized.height
            );
            return false;
        }

        client.window->requestResize(640, 480);
        pump(*client.platform);
        const Reply geometry = command(fd, Command::QueryWindowGeometry);
        if (geometry.first != 640 || geometry.second != 480
            || events.lastInfo.width != 640
            || events.lastInfo.height != 480) {
            fprintf(stderr, "window API: explicit resize failed\n");
            return false;
        }

        command(fd, Command::CloseWindow);
        pump(*client.platform);
        if (events.closeCount != 1) {
            fprintf(stderr, "window API: duplicate close callback\n");
            return false;
        }
        return true;
    }
}
