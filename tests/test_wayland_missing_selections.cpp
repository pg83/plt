#include "test.h"

#include <stdio.h>

namespace plt::test {
    bool missingSelections(int fd) {
        Client client(fd);
        ReadSink primary;
        ReadSink clipboard;
        client.window->readPrimary(primary);
        client.window->readClipboard(clipboard);
        if (primary.complete || clipboard.complete) {
            fprintf(stderr, "missing selections: callback was synchronous\n");
            return false;
        }
        pump(*client.platform);
        if (!primary.complete || primary.success || !primary.content.empty()
            || !clipboard.complete || clipboard.success
            || !clipboard.content.empty()) {
            fprintf(stderr, "missing selections: failure was not reported\n");
            return false;
        }
        return true;
    }
}
