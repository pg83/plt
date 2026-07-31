#include "test.h"

#include "cursor-shape-v1-server-protocol.h"

#include <stdio.h>

namespace plt::test {
    namespace {
        bool expectShape(Client& client, int fd, PointerIcon icon, u32 shape) {
            client.window->requestPointerIcon(icon);
            pump(*client.platform);
            const Reply cursor = command(fd, Command::QueryCursor);
            if (cursor.first != static_cast<i32>(shape) || cursor.second != 1) {
                fprintf(
                    stderr,
                    "cursor shapes: icon %u produced shape %d instead of %u\n",
                    static_cast<u32>(icon),
                    cursor.first,
                    shape
                );
                return false;
            }
            return true;
        }
    }

    bool cursorShapes(int fd) {
        Client client(fd);
        command(fd, Command::PointerEnter);
        pump(*client.platform);

        if (!expectShape(client, fd, PointerIcon::Default, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT)
            || !expectShape(client, fd, PointerIcon::Text, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT)
            || !expectShape(client, fd, PointerIcon::Grabbing, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING)
            || !expectShape(client, fd, PointerIcon::ResizeNorthEastSouthWest, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NESW_RESIZE)
            || !expectShape(client, fd, PointerIcon::ZoomOut, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ZOOM_OUT)) {
            return false;
        }

        // The server offers cursor-shape v2: the v2 shapes pass through and
        // the Cocoa-only poof cursor maps onto no-drop.
        if (!expectShape(client, fd, PointerIcon::DndAsk, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DND_ASK)
            || !expectShape(client, fd, PointerIcon::ResizeAll, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_RESIZE)
            || !expectShape(client, fd, PointerIcon::DisappearingItem, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NO_DROP)) {
            return false;
        }
        return true;
    }

    bool cursorShapesV1(int fd) {
        if (command(fd, Command::CursorShapeV1).count != 1) {
            fprintf(stderr, "cursor shapes v1: could not downgrade the global\n");
            return false;
        }
        Client client(fd);
        command(fd, Command::PointerEnter);
        pump(*client.platform);

        // Bound at v1, the v2-only shapes must degrade to v1 equivalents
        // while v1 shapes still pass through unchanged.
        if (!expectShape(client, fd, PointerIcon::ZoomIn, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ZOOM_IN)
            || !expectShape(client, fd, PointerIcon::DndAsk, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COPY)
            || !expectShape(client, fd, PointerIcon::ResizeAll, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE)) {
            return false;
        }
        return true;
    }
}
