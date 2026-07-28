#include "test.h"

#include <stdio.h>

namespace plt::test {
    bool rejectedSelection(int fd) {
        Client client(fd);
        command(fd, Command::OfferSelection);
        pump(*client.platform);
        RejectSink read;
        client.window->readClipboard(read);
        if (command(fd, Command::ReleaseRead).count != 1) {
            fprintf(stderr, "rejected selection: no transfer fd\n");
            return false;
        }
        pump(*client.platform);
        if (read.dataCount != 1 || read.doneCount != 1 || read.success) {
            fprintf(stderr, "rejected selection: cancellation contract failed\n");
            return false;
        }
        return true;
    }
}
