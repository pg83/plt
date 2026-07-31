#include "test.h"

#include <stdio.h>

namespace plt::test {
    bool plainMimeSelection(int fd) {
        Client client(fd);
        if (command(fd, Command::OfferPlainSelection).count != 1) {
            fprintf(stderr, "plain MIME: data device was not ready\n");
            return false;
        }
        pump(*client.platform);

        ReadSink read;
        client.window->secondary()->read(read);
        if (command(fd, Command::ReleaseRead).count != 1) {
            fprintf(stderr, "plain MIME: fallback was not requested\n");
            return false;
        }
        pump(*client.platform);
        if (!read.complete || !read.success
            || stl::StringView(read.content)
                != stl::StringView(u8"hermetic Wayland clipboard")) {
            fprintf(stderr, "plain MIME: fallback read failed\n");
            return false;
        }
        return true;
    }
}
