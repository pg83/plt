#include "test.h"

#include <stdio.h>

namespace plt::test {
    bool scrollValue120(int fd) {
        InputRecorder input;
        Client client(fd, 800, 1, nullptr, &input);
        command(fd, Command::PointerValue120);
        pump(*client.platform);
        if (input.scrollCount != 1
            || input.lastScroll.x != 0.0
            || input.lastScroll.y != 2.0) {
            fprintf(
                stderr,
                "value120 scroll: expected 2 wheel lines, got %u events x=%f y=%f\n",
                input.scrollCount,
                input.lastScroll.x,
                input.lastScroll.y
            );
            return false;
        }
        return true;
    }
}
