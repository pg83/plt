#include "test.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <wayland-client-core.h>

namespace plt::test {
    bool queuedWaylandEvent(int fd) {
        Platform* platform = nullptr;
        StopOnClose events(platform);
        Client client(fd, 800, 1, &events);
        platform = client.platform;
        if (command(fd, Command::CloseWindow).count != 1) {
            fprintf(stderr, "queued event: close was not sent\n");
            return false;
        }

        auto* const display = static_cast<wl_display*>(
            client.window->renderContext().connection
        );
        if (wl_display_prepare_read(display) != 0) {
            fprintf(stderr, "queued event: client queue was not empty\n");
            return false;
        }
        pollfd source{
            .fd = wl_display_get_fd(display),
            .events = POLLIN,
            .revents = 0,
        };
        int result;
        do {
            result = poll(&source, 1, 1000);
        } while (result < 0 && errno == EINTR);
        if (result <= 0 || !(source.revents & POLLIN)
            || wl_display_read_events(display) < 0) {
            wl_display_cancel_read(display);
            fprintf(stderr, "queued event: could not queue close event\n");
            return false;
        }

        client.platform->run();
        if (!events.closed) {
            fprintf(stderr, "queued event: close callback stalled\n");
            return false;
        }
        return true;
    }
}
