#include "test.h"

#include <stdio.h>

namespace plt::test {
    bool multipleWindows(int fd) {
        EventSink firstEvents;
        InputRecorder firstInput;
        Client client(fd, 800, 1, &firstEvents, &firstInput);
        const u32 firstResizes = firstEvents.resizeCount;

        EventSink secondEvents;
        InputRecorder secondInput;
        Window* const second = client.platform->createWindow(
            *client.owner,
            {
                .appId = stl::StringView(u8"plt.integration.second"),
                .title = stl::StringView(u8"second"),
                .width = 640,
                .height = 480,
                .minimumWidth = 1,
                .minimumHeight = 1,
                .input = &secondInput,
                .events = &secondEvents,
            }
        );
        second->show();
        for (u32 attempt = 0; attempt != 10
             && secondEvents.resizeCount == 0; ++attempt) {
            pump(*client.platform);
        }
        if (secondEvents.resizeCount == 0
            || second->info().width != 640
            || client.window->info().width != 800
            || firstEvents.resizeCount != firstResizes) {
            fprintf(stderr, "multiple windows: initial state leaked\n");
            return false;
        }

        command(fd, Command::ConfigureWindowResize);
        command(fd, Command::PointerEnter);
        pump(*client.platform);
        if (second->info().width != 819
            || client.window->info().width != 800
            || secondInput.pointerEnterCount != 1
            || firstInput.pointerEnterCount != 0) {
            fprintf(
                stderr,
                "multiple windows: widths=%u/%u enters=%u/%u\n",
                client.window->info().width,
                second->info().width,
                firstInput.pointerEnterCount,
                secondInput.pointerEnterCount
            );
            return false;
        }
        return true;
    }
}
