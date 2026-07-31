#include "test.h"

#include <stdio.h>

namespace plt::test {
    bool cancelAsynchronousRead(int fd) {
        Client client(fd);
        command(fd, Command::OfferSelection);
        pump(*client.platform);

        ReadSink read;
        client.window->secondary()->read(read);
        client.window->secondary()->cancel(read);
        const bool completeAfterCancel = read.complete;
        const bool successAfterCancel = read.success;
        const size_t bytesAfterCancel = read.content.length();
        if (command(fd, Command::ReleaseRead).count != 1) {
            fprintf(stderr, "cancel read: no transfer fd was available\n");
            return false;
        }
        pump(*client.platform);
        if (read.complete != completeAfterCancel
            || read.success != successAfterCancel
            || read.content.length() != bytesAfterCancel) {
            fprintf(
                stderr,
                "cancel read: callback arrived after cancel returned\n"
            );
            return false;
        }
        return true;
    }
}
