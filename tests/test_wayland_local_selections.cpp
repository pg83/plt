#include "test.h"

#include <stdio.h>

namespace plt::test {
    bool localSelections(int fd) {
        Client client(fd);
        command(fd, Command::PointerEnter);
        pump(*client.platform);

        client.window->primary()->write(stl::StringView(u8"primary content"));
        client.window->secondary()->write(stl::StringView(u8"clipboard content"));
        pump(*client.platform);
        if (command(fd, Command::QueryPrimarySelection).count != 1
            || command(fd, Command::QuerySelection).count != 1) {
            fprintf(stderr, "local selections: ownership was not published\n");
            return false;
        }

        ReadSink primary;
        ReadSink clipboard;
        client.window->primary()->read(primary);
        client.window->secondary()->read(clipboard);
        if (primary.complete || clipboard.complete) {
            fprintf(stderr, "local selections: callback was synchronous\n");
            return false;
        }
        pump(*client.platform);
        if (!primary.complete || !primary.success
            || stl::StringView(primary.content) != stl::StringView(u8"primary content")
            || !clipboard.complete || !clipboard.success
            || stl::StringView(clipboard.content)
                != stl::StringView(u8"clipboard content")) {
            fprintf(stderr, "local selections: contents mismatch\n");
            return false;
        }

        ReadSink cancelled;
        client.window->primary()->read(cancelled);
        client.window->primary()->cancel(cancelled);
        pump(*client.platform);
        if (cancelled.complete || !cancelled.content.empty()) {
            fprintf(stderr, "local selections: cancelled callback was delivered\n");
            return false;
        }
        return true;
    }
}
