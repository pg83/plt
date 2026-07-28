#include "test.h"

#include <stdio.h>

namespace plt::test {
    namespace {
        struct CancelReadOnReady final: ClipboardRead {
            CancelReadOnReady(
                Platform& platform_,
                Window& window_,
                ClipboardRead& read_
            )
                : platform(platform_)
                , window(window_)
                , read(read_)
            {
            }

            bool data(stl::StringView) override {
                dataCalled = true;
                window.cancelClipboardRead(read);
                return true;
            }

            void done(bool success_) override {
                complete = true;
                success = success_;
                platform.stop();
            }

            Platform& platform;
            Window& window;
            ClipboardRead& read;
            bool dataCalled = false;
            bool complete = false;
            bool success = false;
        };
    }

    bool cancelReadyClipboardRead(int fd) {
        Client client(fd);
        client.window->writeClipboard(stl::StringView(u8"local clipboard"));
        command(fd, Command::PointerEnter);
        pump(*client.platform);

        ReadSink cancelled;
        CancelReadOnReady cancel(
            *client.platform,
            *client.window,
            cancelled
        );
        client.window->readClipboard(cancel);
        client.window->readClipboard(cancelled);
        client.platform->run();

        if (!cancel.dataCalled || !cancel.complete || !cancel.success
            || cancelled.complete || !cancelled.content.empty()) {
            fprintf(
                stderr,
                "cancel ready read: stale transfer callback was delivered\n"
            );
            return false;
        }
        return true;
    }
}
