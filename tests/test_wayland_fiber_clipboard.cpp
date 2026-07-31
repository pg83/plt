#include "test.h"

#include "fiber.h"

#include <std/thr/runable.h>

#include <stdio.h>

namespace plt::test {
    bool fiberClipboard(int fd) {
        Client client(fd);
        Scheduler* const scheduler = client.platform->scheduler();
        command(fd, Command::PointerEnter);
        pump(*client.platform);

        // A remote offer blocks the fiber on the transfer pipe while the
        // loop keeps running; nested calls read the clipboard mid-stack.
        command(fd, Command::OfferSelection);
        pump(*client.platform);
        stl::Buffer remote;
        bool remoteSuccess = false;
        bool remoteComplete = false;
        auto remoteBody = stl::makeRunable([&] {
            remoteSuccess = client.window->secondary()->readAll(remote);
            remoteComplete = true;
        });
        scheduler->spawn(remoteBody);
        if (remoteComplete) {
            fprintf(stderr, "fiber clipboard: remote read completed before any data arrived\n");
            return false;
        }
        if (command(fd, Command::ReleaseRead).count != 1) {
            fprintf(stderr, "fiber clipboard: no transfer fd was available\n");
            return false;
        }
        pump(*client.platform);
        if (!remoteComplete || !remoteSuccess || stl::StringView(remote) != stl::StringView(u8"hermetic Wayland clipboard")) {
            fprintf(stderr, "fiber clipboard: remote selection was not delivered\n");
            return false;
        }

        // Reading a selection this client owns completes without blocking.
        client.window->secondary()->write(stl::StringView(u8"local fiber clipboard"));
        pump(*client.platform);
        stl::Buffer local;
        bool localSuccess = false;
        bool localComplete = false;
        auto localBody = stl::makeRunable([&] {
            localSuccess = client.window->secondary()->readAll(local);
            localComplete = true;
        });
        scheduler->spawn(localBody);
        if (!localComplete || !localSuccess || stl::StringView(local) != stl::StringView(u8"local fiber clipboard")) {
            fprintf(stderr, "fiber clipboard: local selection was not read inline\n");
            return false;
        }

        // Outside a fiber the blocking call must refuse instead of stalling
        // the loop.
        stl::Buffer outside;
        if (client.window->secondary()->readAll(outside)) {
            fprintf(stderr, "fiber clipboard: readAll succeeded outside a fiber\n");
            return false;
        }
        return true;
    }
}
