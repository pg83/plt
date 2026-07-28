#include "test.h"

#include <stdio.h>

namespace plt::test {
    bool frameApi(int fd) {
        EventSink events;
        Client client(fd, 800, 1, &events);
        if (!client.window->requestFrame() || !client.window->requestFrame()) {
            fprintf(stderr, "frame API: requestFrame failed\n");
            return false;
        }
        pump(*client.platform);
        Reply frames = command(fd, Command::QueryFrames);
        if (frames.count != 1 || frames.first != 1) {
            fprintf(stderr, "frame API: duplicate request was sent\n");
            return false;
        }

        client.window->cancelFrame();
        command(fd, Command::CompleteFrames);
        pump(*client.platform);
        if (events.frameCount != 0) {
            fprintf(stderr, "frame API: cancelled callback was delivered\n");
            return false;
        }

        if (!client.window->requestFrame()) {
            fprintf(stderr, "frame API: second request failed\n");
            return false;
        }
        pump(*client.platform);
        frames = command(fd, Command::QueryFrames);
        if (frames.count != 2 || frames.first != 1) {
            fprintf(stderr, "frame API: second request was not sent\n");
            return false;
        }
        command(fd, Command::CompleteFrames);
        pump(*client.platform);
        client.window->cancelFrame();
        if (events.frameCount != 1) {
            fprintf(stderr, "frame API: completion was not delivered\n");
            return false;
        }
        return true;
    }
}
