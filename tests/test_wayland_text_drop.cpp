#include "test.h"

#include <stdio.h>

namespace plt::test {
    bool textDrop(int fd) {
        InputRecorder input;
        Client client(fd, 800, 1, nullptr, &input);
        if (command(fd, Command::DragEnter).count != 1) {
            fprintf(stderr, "text drop: drag enter was not delivered\n");
            return false;
        }
        pump(*client.platform);

        const Reply accept = command(fd, Command::QueryDragAccept);
        if (accept.count == 0 || accept.first != 1 || accept.second != 1) {
            fprintf(
                stderr,
                "text drop: offer was not accepted with the utf-8 mime and the copy action\n"
            );
            return false;
        }

        command(fd, Command::DragDrop);
        pump(*client.platform);
        if (command(fd, Command::DragData).count != 1) {
            fprintf(stderr, "text drop: no transfer fd was available after drop\n");
            return false;
        }
        pump(*client.platform);

        if (input.dropCount != 1 || stl::StringView(input.lastDrop) != stl::StringView(u8"hermetic Wayland drop")) {
            fprintf(stderr, "text drop: payload was not delivered to the input sink\n");
            return false;
        }
        const Reply finish = command(fd, Command::QueryDragFinish);
        if (finish.count != 1 || finish.first != 1) {
            fprintf(stderr, "text drop: the offer was not received and finished as utf-8\n");
            return false;
        }
        return true;
    }

    bool utf8StringDrop(int fd) {
        InputRecorder input;
        Client client(fd, 800, 1, nullptr, &input);
        if (command(fd, Command::DragEnterUtf8String).count != 1) {
            fprintf(stderr, "UTF8_STRING drop: drag enter was not delivered\n");
            return false;
        }
        pump(*client.platform);

        // The source offers only UTF8_STRING; accept and receive must name
        // that exact mime instead of a preferred spelling.
        const Reply accept = command(fd, Command::QueryDragAccept);
        if (accept.count == 0 || accept.first != 3) {
            fprintf(stderr, "UTF8_STRING drop: offer was not accepted as UTF8_STRING\n");
            return false;
        }

        command(fd, Command::DragDrop);
        pump(*client.platform);
        if (command(fd, Command::DragData).count != 1) {
            fprintf(stderr, "UTF8_STRING drop: no transfer fd was available after drop\n");
            return false;
        }
        pump(*client.platform);

        if (input.dropCount != 1 || stl::StringView(input.lastDrop) != stl::StringView(u8"hermetic Wayland drop")) {
            fprintf(stderr, "UTF8_STRING drop: payload was not delivered to the input sink\n");
            return false;
        }
        const Reply finish = command(fd, Command::QueryDragFinish);
        if (finish.count != 1 || finish.first != 3) {
            fprintf(stderr, "UTF8_STRING drop: the offer was not received as UTF8_STRING\n");
            return false;
        }
        return true;
    }

    bool cancelledDrag(int fd) {
        InputRecorder input;
        Client client(fd, 800, 1, nullptr, &input);
        command(fd, Command::DragEnter);
        pump(*client.platform);
        command(fd, Command::DragLeave);
        pump(*client.platform);

        // A drop after leave belongs to no session; nothing may be received
        // or delivered.
        command(fd, Command::DragDrop);
        pump(*client.platform);
        if (command(fd, Command::DragData).count != 0) {
            fprintf(stderr, "cancelled drag: a transfer was started after leave\n");
            return false;
        }
        if (input.dropCount != 0) {
            fprintf(stderr, "cancelled drag: a payload was delivered after leave\n");
            return false;
        }
        return true;
    }
}
