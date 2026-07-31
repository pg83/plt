#include "test.h"

#include <stdio.h>

namespace plt::test {
    bool unsupportedMimeSelection(int fd) {
        Client client(fd);
        if (command(fd, Command::OfferUnsupportedSelection).count != 1) {
            fprintf(stderr, "unsupported MIME: data device was not ready\n");
            return false;
        }
        pump(*client.platform);

        ReadSink read;
        client.window->secondary()->read(read);
        if (read.complete) {
            fprintf(stderr, "unsupported MIME: callback was synchronous\n");
            return false;
        }
        pump(*client.platform);
        if (!read.complete || read.success || !read.content.empty()) {
            fprintf(stderr, "unsupported MIME: failure was not reported\n");
            return false;
        }
        return true;
    }
}
