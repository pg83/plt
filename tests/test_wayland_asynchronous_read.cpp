#include "test.h"

#include <stdio.h>

namespace plt::test {
    bool asynchronousRead(int fd) {
        Client client(fd);
        if (command(fd, Command::OfferSelection).count != 1) {
            fprintf(stderr, "async read: data device was not ready\n");
            return false;
        }
        pump(*client.platform);

        ReadSink read;
        client.window->secondary()->read(read);
        const Reply released = command(fd, Command::ReleaseRead);
        if (released.count != 1) {
            fprintf(
                stderr,
                "async read: readClipboard returned before server received receive, but no transfer fd was available\n"
            );
            return false;
        }
        for (unsigned attempt = 0; attempt != 10 && !read.complete; ++attempt) {
            pump(*client.platform);
        }
        if (!read.complete || !read.success
            || stl::StringView(read.content)
                != stl::StringView(u8"hermetic Wayland clipboard")) {
            fprintf(
                stderr,
                "async read: complete=%d success=%d bytes=%zu\n",
                read.complete,
                read.success,
                read.content.length()
            );
            return false;
        }
        return true;
    }
}
