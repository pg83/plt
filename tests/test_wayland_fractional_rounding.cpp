#include "test.h"

#include <stdio.h>

namespace plt::test {
    bool fractionalRounding(int fd) {
        Client client(fd, 802);
        command(fd, Command::PreferredScale);
        pump(*client.platform);
        const u32 width = client.window->info().width;
        if (width != 1003) {
            fprintf(
                stderr,
                "fractional rounding: width=%u, expected 1003\n",
                width
            );
            return false;
        }
        return true;
    }
}
