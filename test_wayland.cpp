#include "platform.h"
#include "poller.h"
#include "input.h"
#include "window.h"

#include "cursor-shape-v1-server-protocol.h"
#include "fractional-scale-v1-server-protocol.h"
#include "primary-selection-unstable-v1-server-protocol.h"
#include "viewporter-server-protocol.h"
#include "xdg-activation-v1-server-protocol.h"
#include "xdg-decoration-unstable-v1-server-protocol.h"
#include "xdg-shell-server-protocol.h"

#include <std/mem/obj_pool.h>
#include <std/alg/minmax.h>
#include <std/lib/buffer.h>
#include <std/lib/vector.h>
#include <std/str/view.h>
#include <std/sys/crt.h>
#include <std/thr/poll_fd.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <xkbcommon/xkbcommon.h>

using namespace stl;

namespace {
    enum class Command : u32 {
        PointerEnter,
        PreferredScale,
        QuerySelection,
        QueryMinimum,
        OfferSelection,
        ReleaseRead,
        RequestSourceData,
        RequestBrokenSourceData,
        ReleaseWrite,
        QueryWrite,
        AwaitTitles,
        ConfigureWindowState,
        ConfigureWindowResize,
        CloseWindow,
        QueryWindowRequests,
        QueryWindowGeometry,
        QueryFrames,
        CompleteFrames,
        PointerSequence,
        QueryCursor,
        KeyboardEnter,
        KeyboardPress,
        KeyboardRelease,
        KeyboardLeave,
        QueryActivation,
        QueryPrimarySelection,
        OfferPrimarySelection,
        RequestPrimarySourceData,
        Quit,
    };

    struct Reply {
        u32 count = 0;
        i32 first = 0;
        i32 second = 0;
    };

    enum WindowRequest : u32 {
        UpdatedTitle = 1 << 0,
        InitialAppId = 1 << 1,
        Move = 1 << 2,
        Maximize = 1 << 3,
        Unmaximize = 1 << 4,
        Fullscreen = 1 << 5,
        Unfullscreen = 1 << 6,
        Minimize = 1 << 7,
    };

    bool transfer(int fd, void* data, size_t size, bool writeData) {
        auto* cursor = static_cast<unsigned char*>(data);
        while (size != 0) {
            const ssize_t count = writeData
                ? write(fd, cursor, size)
                : read(fd, cursor, size);
            if (count > 0) {
                cursor += count;
                size -= static_cast<size_t>(count);
            } else if (count < 0 && errno == EINTR) {
                continue;
            } else {
                return false;
            }
        }
        return true;
    }

    Buffer repeated(size_t size, u8 value) {
        Buffer result(size);
        result.zero(size);
        u8* const bytes = static_cast<u8*>(result.mutData());
        for (size_t index = 0; index != size; ++index) {
            bytes[index] = value;
        }
        return result;
    }

    struct Server;

    struct Surface {
        Server* server = nullptr;
        wl_resource* surface = nullptr;
        wl_resource* xdgSurface = nullptr;
        wl_resource* toplevel = nullptr;
        wl_resource* fractionalScale = nullptr;
        bool configured = false;
    };

    struct Server {
        Server();
        ~Server();

        bool run(int controlFd, pid_t child);
        void handle(Command command, int controlFd);
        void sendInitialConfigure(Surface& surface);
        void sendConfigure(
            Surface& surface,
            i32 width,
            i32 height,
            const u32* states,
            size_t stateCount
        );

        wl_display* display = nullptr;
        wl_event_loop* loop = nullptr;
        wl_client* client = nullptr;
        wl_resource* pointer = nullptr;
        wl_resource* keyboard = nullptr;
        wl_resource* dataDevice = nullptr;
        wl_resource* dataSource = nullptr;
        wl_resource* dataOffer = nullptr;
        wl_resource* primaryDevice = nullptr;
        wl_resource* primarySource = nullptr;
        wl_resource* primaryOffer = nullptr;
        wl_resource* cursorShapeDevice = nullptr;
        Surface* window = nullptr;
        Vector<wl_resource*> frameCallbacks;
        wl_event_source* writeSource = nullptr;
        int readWriteFd = -1;
        int writeReadFd = -1;
        u32 writtenBytes = 0;
        u32 titleCount = 0;
        u32 targetTitleCount = 0;
        u32 serial = 1;
        u32 selectionCount = 0;
        u32 primarySelectionCount = 0;
        u32 requestFlags = 0;
        u32 frameRequestCount = 0;
        u32 cursorShapeCount = 0;
        u32 cursorShape = 0;
        u32 activationCount = 0;
        i32 geometryWidth = 0;
        i32 geometryHeight = 0;
        i32 minimumWidth = 0;
        i32 minimumHeight = 0;
        u32 minimumCount = 0;
    };

    void destroyResource(wl_client*, wl_resource* resource) {
        wl_resource_destroy(resource);
    }

    void bindSimple(
        wl_client* client,
        void*,
        u32 version,
        u32 id,
        const wl_interface* interface,
        const void* implementation,
        void* data
    ) {
        wl_resource* const resource = wl_resource_create(
            client,
            interface,
            static_cast<int>(version),
            id
        );
        wl_resource_set_implementation(resource, implementation, data, nullptr);
    }

    void surfaceDestroy(wl_resource* resource) {
        delete static_cast<Surface*>(wl_resource_get_user_data(resource));
    }

    void surfaceCommit(wl_client*, wl_resource* resource) {
        Surface* const surface =
            static_cast<Surface*>(wl_resource_get_user_data(resource));
        if (surface->xdgSurface != nullptr && surface->toplevel != nullptr
            && !surface->configured) {
            surface->server->sendInitialConfigure(*surface);
        }
    }

    const struct wl_surface_interface surfaceImplementation{
        .destroy = destroyResource,
        .attach = [](wl_client*, wl_resource*, wl_resource*, i32, i32) {},
        .damage = [](wl_client*, wl_resource*, i32, i32, i32, i32) {},
        .frame = [](wl_client* client, wl_resource* resource, u32 id) {
            auto* const surface =
                static_cast<Surface*>(wl_resource_get_user_data(resource));
            wl_resource* const callback =
                wl_resource_create(client, &wl_callback_interface, 1, id);
            surface->server->frameCallbacks.pushBack(callback);
            ++surface->server->frameRequestCount;
        },
        .set_opaque_region = [](wl_client*, wl_resource*, wl_resource*) {},
        .set_input_region = [](wl_client*, wl_resource*, wl_resource*) {},
        .commit = surfaceCommit,
        .set_buffer_transform = [](wl_client*, wl_resource*, i32) {},
        .set_buffer_scale = [](wl_client*, wl_resource*, i32) {},
        .damage_buffer = [](wl_client*, wl_resource*, i32, i32, i32, i32) {},
        .offset = [](wl_client*, wl_resource*, i32, i32) {},
        .get_release = nullptr,
    };

    const struct wl_region_interface regionImplementation{
        .destroy = destroyResource,
        .add = [](wl_client*, wl_resource*, i32, i32, i32, i32) {},
        .subtract = [](wl_client*, wl_resource*, i32, i32, i32, i32) {},
    };

    const struct wl_compositor_interface compositorImplementation{
        .create_surface = [](wl_client* client, wl_resource* resource, u32 id) {
            auto* const server =
                static_cast<Server*>(wl_resource_get_user_data(resource));
            wl_resource* const surfaceResource = wl_resource_create(
                client,
                &wl_surface_interface,
                min(6, wl_resource_get_version(resource)),
                id
            );
            auto* const surface = new Surface{
                .server = server,
                .surface = surfaceResource,
            };
            server->window = surface;
            wl_resource_set_implementation(
                surfaceResource,
                &surfaceImplementation,
                surface,
                surfaceDestroy
            );
        },
        .create_region = [](wl_client* client, wl_resource* resource, u32 id) {
            wl_resource* const region = wl_resource_create(
                client,
                &wl_region_interface,
                wl_resource_get_version(resource),
                id
            );
            wl_resource_set_implementation(
                region,
                &regionImplementation,
                nullptr,
                nullptr
            );
        },
        .release = destroyResource,
    };

    const struct wl_pointer_interface pointerImplementation{
        .set_cursor = [](wl_client*, wl_resource*, u32, wl_resource*, i32, i32) {},
        .release = destroyResource,
    };

    void sendKeymap(Server& server) {
        xkb_context* const context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        xkb_rule_names names{};
        xkb_keymap* const keymap = context == nullptr
            ? nullptr
            : xkb_keymap_new_from_names(
                context,
                &names,
                XKB_KEYMAP_COMPILE_NO_FLAGS
            );
        char* const text = keymap == nullptr
            ? nullptr
            : xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
        if (text != nullptr) {
            const size_t size = strLen(reinterpret_cast<const u8*>(text)) + 1;
            const int fd = memfd_create("plt-keymap", MFD_CLOEXEC);
            if (fd >= 0 && transfer(fd, text, size, true)
                && lseek(fd, 0, SEEK_SET) == 0) {
                wl_keyboard_send_keymap(
                    server.keyboard,
                    WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
                    fd,
                    static_cast<u32>(size)
                );
            }
            if (fd >= 0) {
                close(fd);
            }
            free(text);
        }
        if (keymap != nullptr) {
            xkb_keymap_unref(keymap);
        }
        if (context != nullptr) {
            xkb_context_unref(context);
        }
    }

    const struct wl_keyboard_interface keyboardImplementation{
        .release = destroyResource,
    };

    const struct wl_seat_interface seatImplementation{
        .get_pointer = [](wl_client* client, wl_resource* resource, u32 id) {
            auto* const server =
                static_cast<Server*>(wl_resource_get_user_data(resource));
            server->pointer = wl_resource_create(
                client,
                &wl_pointer_interface,
                min(8, wl_resource_get_version(resource)),
                id
            );
            wl_resource_set_implementation(
                server->pointer,
                &pointerImplementation,
                server,
                nullptr
            );
        },
        .get_keyboard = [](wl_client* client, wl_resource* resource, u32 id) {
            auto* const server =
                static_cast<Server*>(wl_resource_get_user_data(resource));
            server->keyboard = wl_resource_create(
                client,
                &wl_keyboard_interface,
                min(8, wl_resource_get_version(resource)),
                id
            );
            wl_resource_set_implementation(
                server->keyboard,
                &keyboardImplementation,
                server,
                nullptr
            );
            sendKeymap(*server);
            wl_keyboard_send_repeat_info(server->keyboard, 1000, 1);
        },
        .get_touch = nullptr,
        .release = destroyResource,
    };

    const struct wl_data_source_interface dataSourceImplementation{
        .offer = [](wl_client*, wl_resource*, const char*) {},
        .destroy = [](wl_client*, wl_resource* resource) {
            auto* const server =
                static_cast<Server*>(wl_resource_get_user_data(resource));
            if (server->dataSource == resource) {
                server->dataSource = nullptr;
            }
            wl_resource_destroy(resource);
        },
        .set_actions = [](wl_client*, wl_resource*, u32) {},
    };

    const struct wl_data_offer_interface dataOfferImplementation{
        .accept = [](wl_client*, wl_resource*, u32, const char*) {},
        .receive = [](wl_client*, wl_resource* resource, const char*, i32 fd) {
            auto* const server =
                static_cast<Server*>(wl_resource_get_user_data(resource));
            if (server->readWriteFd != -1) {
                close(server->readWriteFd);
            }
            server->readWriteFd = fd;
        },
        .destroy = [](wl_client*, wl_resource* resource) {
            auto* const server =
                static_cast<Server*>(wl_resource_get_user_data(resource));
            if (server->dataOffer == resource) {
                server->dataOffer = nullptr;
            }
            wl_resource_destroy(resource);
        },
        .finish = [](wl_client*, wl_resource*) {},
        .set_actions = [](wl_client*, wl_resource*, u32, u32) {},
    };

    const struct wl_data_device_interface dataDeviceImplementation{
        .start_drag = nullptr,
        .set_selection = [](wl_client*, wl_resource* resource, wl_resource*, u32) {
            auto* const server =
                static_cast<Server*>(wl_resource_get_user_data(resource));
            ++server->selectionCount;
        },
        .release = destroyResource,
    };

    const struct wl_data_device_manager_interface dataManagerImplementation{
        .create_data_source =
            [](wl_client* client, wl_resource* resource, u32 id) {
            auto* const server =
                static_cast<Server*>(wl_resource_get_user_data(resource));
            wl_resource* const source = wl_resource_create(
                client,
                &wl_data_source_interface,
                wl_resource_get_version(resource),
                id
            );
            server->dataSource = source;
            wl_resource_set_implementation(
                source,
                &dataSourceImplementation,
                server,
                nullptr
            );
        },
        .get_data_device =
            [](wl_client* client, wl_resource* resource, u32 id, wl_resource*) {
            auto* const server =
                static_cast<Server*>(wl_resource_get_user_data(resource));
            server->dataDevice = wl_resource_create(
                client,
                &wl_data_device_interface,
                wl_resource_get_version(resource),
                id
            );
            wl_resource_set_implementation(
                server->dataDevice,
                &dataDeviceImplementation,
                server,
                nullptr
            );
        },
        .release = destroyResource,
    };

    const struct zwp_primary_selection_source_v1_interface primarySourceImplementation{
        .offer = [](wl_client*, wl_resource*, const char*) {},
        .destroy = [](wl_client*, wl_resource* resource) {
            auto* const server =
                static_cast<Server*>(wl_resource_get_user_data(resource));
            if (server->primarySource == resource) {
                server->primarySource = nullptr;
            }
            wl_resource_destroy(resource);
        },
    };

    const struct zwp_primary_selection_offer_v1_interface primaryOfferImplementation{
        .receive =
            [](wl_client*, wl_resource* resource, const char*, i32 fd) {
                auto* const server =
                    static_cast<Server*>(wl_resource_get_user_data(resource));
                if (server->readWriteFd != -1) {
                    close(server->readWriteFd);
                }
                server->readWriteFd = fd;
            },
        .destroy = [](wl_client*, wl_resource* resource) {
            auto* const server =
                static_cast<Server*>(wl_resource_get_user_data(resource));
            if (server->primaryOffer == resource) {
                server->primaryOffer = nullptr;
            }
            wl_resource_destroy(resource);
        },
    };

    const struct zwp_primary_selection_device_v1_interface primaryDeviceImplementation{
        .set_selection =
            [](wl_client*, wl_resource* resource, wl_resource*, u32) {
                auto* const server =
                    static_cast<Server*>(wl_resource_get_user_data(resource));
                ++server->primarySelectionCount;
            },
        .destroy = destroyResource,
    };

    const struct zwp_primary_selection_device_manager_v1_interface
        primaryManagerImplementation{
            .create_source =
                [](wl_client* client, wl_resource* resource, u32 id) {
                    auto* const server =
                        static_cast<Server*>(wl_resource_get_user_data(resource));
                    server->primarySource = wl_resource_create(
                        client,
                        &zwp_primary_selection_source_v1_interface,
                        1,
                        id
                    );
                    wl_resource_set_implementation(
                        server->primarySource,
                        &primarySourceImplementation,
                        server,
                        nullptr
                    );
                },
            .get_device =
                [](wl_client* client, wl_resource* resource, u32 id, wl_resource*) {
                    auto* const server =
                        static_cast<Server*>(wl_resource_get_user_data(resource));
                    server->primaryDevice = wl_resource_create(
                        client,
                        &zwp_primary_selection_device_v1_interface,
                        1,
                        id
                    );
                    wl_resource_set_implementation(
                        server->primaryDevice,
                        &primaryDeviceImplementation,
                        server,
                        nullptr
                    );
                },
            .destroy = destroyResource,
        };

    const struct xdg_toplevel_interface toplevelImplementation{
        .destroy = destroyResource,
        .set_parent = [](wl_client*, wl_resource*, wl_resource*) {},
        .set_title = [](wl_client*, wl_resource* resource, const char* title) {
            auto* const surface =
                static_cast<Surface*>(wl_resource_get_user_data(resource));
            Server& server = *surface->server;
            ++server.titleCount;
            if (StringView(title) == StringView(u8"updated title")) {
                server.requestFlags |= UpdatedTitle;
            }
            if (server.targetTitleCount != 0
                && server.titleCount >= server.targetTitleCount) {
                xdg_toplevel_send_close(resource);
                wl_display_flush_clients(server.display);
                server.targetTitleCount = 0;
            }
        },
        .set_app_id = [](wl_client*, wl_resource* resource, const char* appId) {
            auto* const surface =
                static_cast<Surface*>(wl_resource_get_user_data(resource));
            if (StringView(appId) == StringView(u8"plt.integration")) {
                surface->server->requestFlags |= InitialAppId;
            }
        },
        .show_window_menu = [](wl_client*, wl_resource*, wl_resource*, u32, i32, i32) {},
        .move = [](wl_client*, wl_resource* resource, wl_resource*, u32) {
            auto* const surface =
                static_cast<Surface*>(wl_resource_get_user_data(resource));
            surface->server->requestFlags |= Move;
        },
        .resize = [](wl_client*, wl_resource*, wl_resource*, u32, u32) {},
        .set_max_size = [](wl_client*, wl_resource*, i32, i32) {},
        .set_min_size = [](wl_client*, wl_resource* resource, i32 width, i32 height) {
            auto* const surface =
                static_cast<Surface*>(wl_resource_get_user_data(resource));
            surface->server->minimumWidth = width;
            surface->server->minimumHeight = height;
            ++surface->server->minimumCount;
        },
        .set_maximized = [](wl_client*, wl_resource* resource) {
            auto* const surface =
                static_cast<Surface*>(wl_resource_get_user_data(resource));
            surface->server->requestFlags |= Maximize;
        },
        .unset_maximized = [](wl_client*, wl_resource* resource) {
            auto* const surface =
                static_cast<Surface*>(wl_resource_get_user_data(resource));
            surface->server->requestFlags |= Unmaximize;
        },
        .set_fullscreen =
            [](wl_client*, wl_resource* resource, wl_resource*) {
                auto* const surface =
                    static_cast<Surface*>(wl_resource_get_user_data(resource));
                surface->server->requestFlags |= Fullscreen;
            },
        .unset_fullscreen = [](wl_client*, wl_resource* resource) {
            auto* const surface =
                static_cast<Surface*>(wl_resource_get_user_data(resource));
            surface->server->requestFlags |= Unfullscreen;
        },
        .set_minimized = [](wl_client*, wl_resource* resource) {
            auto* const surface =
                static_cast<Surface*>(wl_resource_get_user_data(resource));
            surface->server->requestFlags |= Minimize;
        },
    };

    const struct xdg_surface_interface xdgSurfaceImplementation{
        .destroy = destroyResource,
        .get_toplevel = [](wl_client* client, wl_resource* resource, u32 id) {
            auto* const surface =
                static_cast<Surface*>(wl_resource_get_user_data(resource));
            surface->toplevel = wl_resource_create(
                client,
                &xdg_toplevel_interface,
                wl_resource_get_version(resource),
                id
            );
            wl_resource_set_implementation(
                surface->toplevel,
                &toplevelImplementation,
                surface,
                nullptr
            );
        },
        .get_popup = nullptr,
        .set_window_geometry =
            [](wl_client*, wl_resource* resource, i32, i32, i32 width, i32 height) {
                auto* const surface =
                    static_cast<Surface*>(wl_resource_get_user_data(resource));
                surface->server->geometryWidth = width;
                surface->server->geometryHeight = height;
            },
        .ack_configure = [](wl_client*, wl_resource*, u32) {},
    };

    const struct xdg_wm_base_interface wmBaseImplementation{
        .destroy = destroyResource,
        .create_positioner = nullptr,
        .get_xdg_surface =
            [](wl_client* client, wl_resource* resource, u32 id, wl_resource* wlSurface) {
            auto* const surface =
                static_cast<Surface*>(wl_resource_get_user_data(wlSurface));
            surface->xdgSurface = wl_resource_create(
                client,
                &xdg_surface_interface,
                wl_resource_get_version(resource),
                id
            );
            wl_resource_set_implementation(
                surface->xdgSurface,
                &xdgSurfaceImplementation,
                surface,
                nullptr
            );
        },
        .pong = [](wl_client*, wl_resource*, u32) {},
    };

    const struct wp_viewport_interface viewportImplementation{
        .destroy = destroyResource,
        .set_source = [](wl_client*, wl_resource*, wl_fixed_t, wl_fixed_t, wl_fixed_t, wl_fixed_t) {},
        .set_destination = [](wl_client*, wl_resource*, i32, i32) {},
    };

    const struct wp_viewporter_interface viewporterImplementation{
        .destroy = destroyResource,
        .get_viewport =
            [](wl_client* client, wl_resource*, u32 id, wl_resource*) {
            wl_resource* const viewport =
                wl_resource_create(client, &wp_viewport_interface, 1, id);
            wl_resource_set_implementation(
                viewport,
                &viewportImplementation,
                nullptr,
                nullptr
            );
        },
    };

    const struct wp_fractional_scale_v1_interface fractionalScaleImplementation{
        .destroy = destroyResource,
    };

    const struct wp_fractional_scale_manager_v1_interface fractionalManagerImplementation{
        .destroy = destroyResource,
        .get_fractional_scale =
            [](wl_client* client, wl_resource*, u32 id, wl_resource* wlSurface) {
            auto* const surface =
                static_cast<Surface*>(wl_resource_get_user_data(wlSurface));
            surface->fractionalScale = wl_resource_create(
                client,
                &wp_fractional_scale_v1_interface,
                1,
                id
            );
            wl_resource_set_implementation(
                surface->fractionalScale,
                &fractionalScaleImplementation,
                surface,
                nullptr
            );
        },
    };

    const struct wp_cursor_shape_device_v1_interface cursorShapeDeviceImplementation{
        .destroy = destroyResource,
        .set_shape = [](wl_client*, wl_resource* resource, u32, u32 shape) {
            auto* const server =
                static_cast<Server*>(wl_resource_get_user_data(resource));
            ++server->cursorShapeCount;
            server->cursorShape = shape;
        },
    };

    const struct wp_cursor_shape_manager_v1_interface cursorShapeManagerImplementation{
        .destroy = destroyResource,
        .get_pointer =
            [](wl_client* client, wl_resource* resource, u32 id, wl_resource*) {
                auto* const server =
                    static_cast<Server*>(wl_resource_get_user_data(resource));
                server->cursorShapeDevice = wl_resource_create(
                    client,
                    &wp_cursor_shape_device_v1_interface,
                    1,
                    id
                );
                wl_resource_set_implementation(
                    server->cursorShapeDevice,
                    &cursorShapeDeviceImplementation,
                    server,
                    nullptr
                );
            },
        .get_tablet_tool_v2 = nullptr,
    };

    const struct xdg_activation_token_v1_interface activationTokenImplementation{
        .set_serial = [](wl_client*, wl_resource*, u32, wl_resource*) {},
        .set_app_id = [](wl_client*, wl_resource*, const char*) {},
        .set_surface = [](wl_client*, wl_resource*, wl_resource*) {},
        .commit = [](wl_client*, wl_resource* resource) {
            xdg_activation_token_v1_send_done(resource, "plt-test-token");
        },
        .destroy = destroyResource,
    };

    const struct xdg_activation_v1_interface activationImplementation{
        .destroy = destroyResource,
        .get_activation_token =
            [](wl_client* client, wl_resource* resource, u32 id) {
                wl_resource* const token = wl_resource_create(
                    client,
                    &xdg_activation_token_v1_interface,
                    1,
                    id
                );
                wl_resource_set_implementation(
                    token,
                    &activationTokenImplementation,
                    wl_resource_get_user_data(resource),
                    nullptr
                );
            },
        .activate =
            [](wl_client*, wl_resource* resource, const char*, wl_resource*) {
                auto* const server =
                    static_cast<Server*>(wl_resource_get_user_data(resource));
                ++server->activationCount;
            },
    };

    const struct zxdg_toplevel_decoration_v1_interface decorationImplementation{
        .destroy = destroyResource,
        .set_mode = [](wl_client*, wl_resource*, u32) {},
        .unset_mode = [](wl_client*, wl_resource*) {},
    };

    const struct zxdg_decoration_manager_v1_interface decorationManagerImplementation{
        .destroy = destroyResource,
        .get_toplevel_decoration =
            [](wl_client* client, wl_resource*, u32 id, wl_resource*) {
                wl_resource* const decoration = wl_resource_create(
                    client,
                    &zxdg_toplevel_decoration_v1_interface,
                    1,
                    id
                );
                wl_resource_set_implementation(
                    decoration,
                    &decorationImplementation,
                    nullptr,
                    nullptr
                );
            },
    };

    void bindCompositor(
        wl_client* client,
        void* data,
        u32 version,
        u32 id
    ) {
        bindSimple(
            client,
            nullptr,
            version,
            id,
            &wl_compositor_interface,
            &compositorImplementation,
            data
        );
    }

    void bindSeat(wl_client* client, void* data, u32 version, u32 id) {
        wl_resource* const resource =
            wl_resource_create(client, &wl_seat_interface, version, id);
        wl_resource_set_implementation(
            resource,
            &seatImplementation,
            data,
            nullptr
        );
        wl_seat_send_capabilities(
            resource,
            WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD
        );
        if (version >= WL_SEAT_NAME_SINCE_VERSION) {
            wl_seat_send_name(resource, "plt-test-seat");
        }
    }

    void bindDataManager(
        wl_client* client,
        void* data,
        u32 version,
        u32 id
    ) {
        bindSimple(
            client,
            nullptr,
            version,
            id,
            &wl_data_device_manager_interface,
            &dataManagerImplementation,
            data
        );
    }

    void bindWmBase(
        wl_client* client,
        void* data,
        u32 version,
        u32 id
    ) {
        bindSimple(
            client,
            nullptr,
            version,
            id,
            &xdg_wm_base_interface,
            &wmBaseImplementation,
            data
        );
    }

    void bindViewporter(
        wl_client* client,
        void* data,
        u32 version,
        u32 id
    ) {
        bindSimple(
            client,
            nullptr,
            version,
            id,
            &wp_viewporter_interface,
            &viewporterImplementation,
            data
        );
    }

    void bindFractional(
        wl_client* client,
        void* data,
        u32 version,
        u32 id
    ) {
        bindSimple(
            client,
            nullptr,
            version,
            id,
            &wp_fractional_scale_manager_v1_interface,
            &fractionalManagerImplementation,
            data
        );
    }

    void bindPrimary(
        wl_client* client,
        void* data,
        u32 version,
        u32 id
    ) {
        bindSimple(
            client,
            nullptr,
            version,
            id,
            &zwp_primary_selection_device_manager_v1_interface,
            &primaryManagerImplementation,
            data
        );
    }

    void bindCursorShape(
        wl_client* client,
        void* data,
        u32 version,
        u32 id
    ) {
        bindSimple(
            client,
            nullptr,
            version,
            id,
            &wp_cursor_shape_manager_v1_interface,
            &cursorShapeManagerImplementation,
            data
        );
    }

    void bindActivation(
        wl_client* client,
        void* data,
        u32 version,
        u32 id
    ) {
        bindSimple(
            client,
            nullptr,
            version,
            id,
            &xdg_activation_v1_interface,
            &activationImplementation,
            data
        );
    }

    void bindDecoration(
        wl_client* client,
        void* data,
        u32 version,
        u32 id
    ) {
        bindSimple(
            client,
            nullptr,
            version,
            id,
            &zxdg_decoration_manager_v1_interface,
            &decorationManagerImplementation,
            data
        );
    }

    void bindOutput(
        wl_client* client,
        void*,
        u32 version,
        u32 id
    ) {
        static const struct wl_output_interface outputImplementation{
            .release = destroyResource,
        };
        wl_resource* const resource =
            wl_resource_create(client, &wl_output_interface, version, id);
        wl_resource_set_implementation(
            resource,
            &outputImplementation,
            nullptr,
            nullptr
        );
        wl_output_send_geometry(
            resource,
            0,
            0,
            300,
            200,
            WL_OUTPUT_SUBPIXEL_UNKNOWN,
            "plt",
            "test",
            WL_OUTPUT_TRANSFORM_NORMAL
        );
        wl_output_send_mode(
            resource,
            WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED,
            1920,
            1080,
            60000
        );
        if (version >= WL_OUTPUT_SCALE_SINCE_VERSION) {
            wl_output_send_scale(resource, 1);
        }
        if (version >= WL_OUTPUT_DONE_SINCE_VERSION) {
            wl_output_send_done(resource);
        }
    }

    Server::Server() {
        display = wl_display_create();
        loop = wl_display_get_event_loop(display);
        wl_global_create(display, &wl_compositor_interface, 6, this, bindCompositor);
        wl_global_create(display, &wl_seat_interface, 8, this, bindSeat);
        wl_global_create(
            display,
            &wl_data_device_manager_interface,
            3,
            this,
            bindDataManager
        );
        wl_global_create(display, &xdg_wm_base_interface, 6, this, bindWmBase);
        wl_global_create(display, &wp_viewporter_interface, 1, this, bindViewporter);
        wl_global_create(
            display,
            &wp_fractional_scale_manager_v1_interface,
            1,
            this,
            bindFractional
        );
        wl_global_create(
            display,
            &zwp_primary_selection_device_manager_v1_interface,
            1,
            this,
            bindPrimary
        );
        wl_global_create(
            display,
            &wp_cursor_shape_manager_v1_interface,
            1,
            this,
            bindCursorShape
        );
        wl_global_create(
            display,
            &xdg_activation_v1_interface,
            1,
            this,
            bindActivation
        );
        wl_global_create(
            display,
            &zxdg_decoration_manager_v1_interface,
            1,
            this,
            bindDecoration
        );
        wl_global_create(display, &wl_output_interface, 4, this, bindOutput);
    }

    Server::~Server() {
        if (writeSource != nullptr) {
            wl_event_source_remove(writeSource);
        }
        if (readWriteFd != -1) {
            close(readWriteFd);
        }
        if (writeReadFd != -1) {
            close(writeReadFd);
        }
        wl_display_destroy_clients(display);
        wl_display_destroy(display);
    }

    void Server::sendInitialConfigure(Surface& surface) {
        sendConfigure(surface, 0, 0, nullptr, 0);
        surface.configured = true;
    }

    void Server::sendConfigure(
        Surface& surface,
        i32 width,
        i32 height,
        const u32* states,
        size_t stateCount
    ) {
        wl_array stateArray;
        wl_array_init(&stateArray);
        for (size_t index = 0; index != stateCount; ++index) {
            auto* const target = static_cast<u32*>(
                wl_array_add(&stateArray, sizeof(u32))
            );
            if (target != nullptr) {
                *target = states[index];
            }
        }
        xdg_toplevel_send_configure(surface.toplevel, width, height, &stateArray);
        xdg_surface_send_configure(surface.xdgSurface, serial++);
        wl_array_release(&stateArray);
        wl_display_flush_clients(display);
    }

    void Server::handle(Command command, int controlFd) {
        Reply reply;
        switch (command) {
            case Command::PointerEnter:
                reply.count = selectionCount;
                if (pointer != nullptr && window != nullptr) {
                    wl_pointer_send_enter(
                        pointer,
                        serial++,
                        window->surface,
                        wl_fixed_from_int(10),
                        wl_fixed_from_int(20)
                    );
                    wl_display_flush_clients(display);
                }
                break;
            case Command::PreferredScale:
                if (window != nullptr && window->fractionalScale != nullptr) {
                    wp_fractional_scale_v1_send_preferred_scale(
                        window->fractionalScale,
                        150
                    );
                    wl_display_flush_clients(display);
                }
                break;
            case Command::QuerySelection:
                reply.count = selectionCount;
                break;
            case Command::QueryMinimum:
                reply.count = minimumCount;
                reply.first = minimumWidth;
                reply.second = minimumHeight;
                break;
            case Command::OfferSelection:
                if (dataDevice != nullptr) {
                    dataOffer = wl_resource_create(
                        client,
                        &wl_data_offer_interface,
                        wl_resource_get_version(dataDevice),
                        0
                    );
                    wl_resource_set_implementation(
                        dataOffer,
                        &dataOfferImplementation,
                        this,
                        nullptr
                    );
                    wl_data_device_send_data_offer(dataDevice, dataOffer);
                    wl_data_offer_send_offer(
                        dataOffer,
                        "text/plain;charset=utf-8"
                    );
                    wl_data_device_send_selection(dataDevice, dataOffer);
                    wl_display_flush_clients(display);
                    reply.count = 1;
                }
                break;
            case Command::ReleaseRead:
                reply.count = readWriteFd != -1;
                if (readWriteFd != -1) {
                    static constexpr char content[] =
                        "hermetic Wayland clipboard";
                    transfer(
                        readWriteFd,
                        const_cast<char*>(content),
                        sizeof(content) - 1,
                        true
                    );
                    close(readWriteFd);
                    readWriteFd = -1;
                }
                break;
            case Command::RequestSourceData:
                if (dataSource != nullptr) {
                    int pipes[2];
                    if (pipe(pipes) == 0) {
                        writeReadFd = pipes[0];
                        wl_data_source_send_send(
                            dataSource,
                            "text/plain;charset=utf-8",
                            pipes[1]
                        );
                        close(pipes[1]);
                        wl_display_flush_clients(display);
                        reply.count = 1;
                    }
                }
                break;
            case Command::RequestBrokenSourceData:
                if (dataSource != nullptr) {
                    int pipes[2];
                    if (pipe(pipes) == 0) {
                        close(pipes[0]);
                        wl_data_source_send_send(
                            dataSource,
                            "text/plain;charset=utf-8",
                            pipes[1]
                        );
                        close(pipes[1]);
                        wl_display_flush_clients(display);
                        reply.count = 1;
                    }
                }
                break;
            case Command::ReleaseWrite:
                if (writeReadFd != -1 && writeSource == nullptr) {
                    fcntl(
                        writeReadFd,
                        F_SETFL,
                        fcntl(writeReadFd, F_GETFL) | O_NONBLOCK
                    );
                    writeSource = wl_event_loop_add_fd(
                        loop,
                        writeReadFd,
                        WL_EVENT_READABLE | WL_EVENT_HANGUP | WL_EVENT_ERROR,
                        [](int fd, u32 mask, void* data) {
                            auto* const server = static_cast<Server*>(data);
                            char buffer[16384];
                            for (;;) {
                                const ssize_t count =
                                    read(fd, buffer, sizeof(buffer));
                                if (count > 0) {
                                    server->writtenBytes +=
                                        static_cast<u32>(count);
                                } else if (count < 0 && errno == EINTR) {
                                    continue;
                                } else if (
                                    count < 0
                                    && (errno == EAGAIN || errno == EWOULDBLOCK)
                                ) {
                                    break;
                                } else {
                                    wl_event_source_remove(server->writeSource);
                                    server->writeSource = nullptr;
                                    close(server->writeReadFd);
                                    server->writeReadFd = -1;
                                    break;
                                }
                            }
                            if (
                                mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)
                                && server->writeSource != nullptr
                            ) {
                                wl_event_source_remove(server->writeSource);
                                server->writeSource = nullptr;
                                close(server->writeReadFd);
                                server->writeReadFd = -1;
                            }
                            return 0;
                        },
                        this
                    );
                    reply.count = 1;
                }
                break;
            case Command::QueryWrite:
                reply.count = writtenBytes;
                reply.first = writeReadFd == -1;
                break;
            case Command::AwaitTitles:
                targetTitleCount = static_cast<u32>(reply.first = 2049);
                reply.count = titleCount;
                if (window != nullptr && window->toplevel != nullptr
                    && titleCount >= targetTitleCount) {
                    xdg_toplevel_send_close(window->toplevel);
                    wl_display_flush_clients(display);
                    targetTitleCount = 0;
                }
                break;
            case Command::ConfigureWindowState:
                if (window != nullptr && window->toplevel != nullptr) {
                    const u32 states[]{
                        XDG_TOPLEVEL_STATE_ACTIVATED,
                        XDG_TOPLEVEL_STATE_MAXIMIZED,
                        XDG_TOPLEVEL_STATE_FULLSCREEN,
                        XDG_TOPLEVEL_STATE_TILED_LEFT,
                    };
                    sendConfigure(
                        *window,
                        900,
                        700,
                        states,
                        sizeof(states) / sizeof(states[0])
                    );
                    reply.count = 1;
                }
                break;
            case Command::ConfigureWindowResize:
                if (window != nullptr && window->toplevel != nullptr) {
                    sendConfigure(*window, 819, 638, nullptr, 0);
                    reply.count = 1;
                }
                break;
            case Command::CloseWindow:
                if (window != nullptr && window->toplevel != nullptr) {
                    xdg_toplevel_send_close(window->toplevel);
                    wl_display_flush_clients(display);
                    reply.count = 1;
                }
                break;
            case Command::QueryWindowRequests:
                reply.count = requestFlags;
                break;
            case Command::QueryWindowGeometry:
                reply.first = geometryWidth;
                reply.second = geometryHeight;
                break;
            case Command::QueryFrames:
                reply.count = frameRequestCount;
                reply.first = static_cast<i32>(frameCallbacks.length());
                break;
            case Command::CompleteFrames:
                reply.count = static_cast<u32>(frameCallbacks.length());
                for (wl_resource* callback : frameCallbacks) {
                    wl_callback_send_done(callback, 1);
                    wl_resource_destroy(callback);
                }
                frameCallbacks.clear();
                wl_display_flush_clients(display);
                break;
            case Command::PointerSequence:
                if (pointer != nullptr && window != nullptr) {
                    wl_pointer_send_enter(
                        pointer,
                        serial++,
                        window->surface,
                        wl_fixed_from_int(10),
                        wl_fixed_from_int(20)
                    );
                    wl_pointer_send_motion(
                        pointer,
                        1000,
                        wl_fixed_from_int(30),
                        wl_fixed_from_int(40)
                    );
                    wl_pointer_send_button(
                        pointer,
                        serial++,
                        1500,
                        BTN_LEFT,
                        WL_POINTER_BUTTON_STATE_PRESSED
                    );
                    wl_pointer_send_axis(
                        pointer,
                        1600,
                        WL_POINTER_AXIS_HORIZONTAL_SCROLL,
                        wl_fixed_from_int(20)
                    );
                    wl_pointer_send_axis(
                        pointer,
                        1600,
                        WL_POINTER_AXIS_VERTICAL_SCROLL,
                        wl_fixed_from_int(-30)
                    );
                    wl_pointer_send_frame(pointer);
                    wl_pointer_send_button(
                        pointer,
                        serial++,
                        1700,
                        BTN_LEFT,
                        WL_POINTER_BUTTON_STATE_RELEASED
                    );
                    wl_pointer_send_frame(pointer);
                    wl_pointer_send_leave(
                        pointer,
                        serial++,
                        window->surface
                    );
                    wl_display_flush_clients(display);
                    reply.count = 1;
                }
                break;
            case Command::QueryCursor:
                reply.count = cursorShapeCount;
                reply.first = static_cast<i32>(cursorShape);
                break;
            case Command::KeyboardEnter:
                if (keyboard != nullptr && window != nullptr) {
                    wl_array keys;
                    wl_array_init(&keys);
                    wl_keyboard_send_enter(
                        keyboard,
                        serial++,
                        window->surface,
                        &keys
                    );
                    wl_keyboard_send_modifiers(
                        keyboard,
                        serial++,
                        0,
                        0,
                        0,
                        0
                    );
                    wl_array_release(&keys);
                    wl_display_flush_clients(display);
                    reply.count = 1;
                }
                break;
            case Command::KeyboardPress:
                if (keyboard != nullptr) {
                    wl_keyboard_send_key(
                        keyboard,
                        serial++,
                        2500,
                        KEY_A,
                        WL_KEYBOARD_KEY_STATE_PRESSED
                    );
                    wl_display_flush_clients(display);
                    reply.count = 1;
                }
                break;
            case Command::KeyboardRelease:
                if (keyboard != nullptr) {
                    wl_keyboard_send_key(
                        keyboard,
                        serial++,
                        2600,
                        KEY_A,
                        WL_KEYBOARD_KEY_STATE_RELEASED
                    );
                    wl_display_flush_clients(display);
                    reply.count = 1;
                }
                break;
            case Command::KeyboardLeave:
                if (keyboard != nullptr && window != nullptr) {
                    wl_keyboard_send_leave(
                        keyboard,
                        serial++,
                        window->surface
                    );
                    wl_display_flush_clients(display);
                    reply.count = 1;
                }
                break;
            case Command::QueryActivation:
                reply.count = activationCount;
                break;
            case Command::QueryPrimarySelection:
                reply.count = primarySelectionCount;
                break;
            case Command::OfferPrimarySelection:
                if (primaryDevice != nullptr) {
                    primaryOffer = wl_resource_create(
                        client,
                        &zwp_primary_selection_offer_v1_interface,
                        1,
                        0
                    );
                    wl_resource_set_implementation(
                        primaryOffer,
                        &primaryOfferImplementation,
                        this,
                        nullptr
                    );
                    zwp_primary_selection_device_v1_send_data_offer(
                        primaryDevice,
                        primaryOffer
                    );
                    zwp_primary_selection_offer_v1_send_offer(
                        primaryOffer,
                        "text/plain;charset=utf-8"
                    );
                    zwp_primary_selection_device_v1_send_selection(
                        primaryDevice,
                        primaryOffer
                    );
                    wl_display_flush_clients(display);
                    reply.count = 1;
                }
                break;
            case Command::RequestPrimarySourceData:
                if (primarySource != nullptr) {
                    int pipes[2];
                    if (pipe(pipes) == 0) {
                        writeReadFd = pipes[0];
                        zwp_primary_selection_source_v1_send_send(
                            primarySource,
                            "text/plain;charset=utf-8",
                            pipes[1]
                        );
                        close(pipes[1]);
                        wl_display_flush_clients(display);
                        reply.count = 1;
                    }
                }
                break;
            case Command::Quit:
                break;
        }
        transfer(controlFd, &reply, sizeof(reply), true);
    }

    bool Server::run(int controlFd, pid_t child) {
        bool controlOpen = true;
        while (controlOpen) {
            pollfd fds[]{
                {
                    .fd = wl_event_loop_get_fd(loop),
                    .events = POLLIN,
                    .revents = 0,
                },
                {
                    .fd = controlFd,
                    .events = POLLIN,
                    .revents = 0,
                },
            };
            int result;
            do {
                result = poll(fds, 2, 5000);
            } while (result < 0 && errno == EINTR);
            if (result <= 0) {
                kill(child, SIGKILL);
                break;
            }
            if (fds[0].revents != 0) {
                if (wl_event_loop_dispatch(loop, 0) < 0) {
                    break;
                }
                wl_display_flush_clients(display);
            }
            if (fds[1].revents & POLLIN) {
                Command command;
                if (!transfer(controlFd, &command, sizeof(command), false)) {
                    controlOpen = false;
                    continue;
                }
                wl_event_loop_dispatch(loop, 0);
                handle(command, controlFd);
                if (command == Command::Quit) {
                    controlOpen = false;
                }
            }
            if (fds[1].revents & (POLLHUP | POLLERR | POLLNVAL)) {
                controlOpen = false;
            }
        }
        int status = 0;
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
        }
        return WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }

    Reply command(int fd, Command commandValue) {
        Reply reply;
        transfer(fd, &commandValue, sizeof(commandValue), true);
        transfer(fd, &reply, sizeof(reply), false);
        return reply;
    }

    struct StopTimer final: plt::TimerCallback {
        explicit StopTimer(plt::Platform& platform_): platform(platform_) {}

        void ready() override {
            platform.stop();
        }

        plt::Platform& platform;
    };

    void pump(plt::Platform& platform) {
        StopTimer stop(platform);
        platform.poller()->timeout(1000, stop);
        platform.run();
    }

    struct Client {
        explicit Client(
            int controlFd_,
            u32 width = 800,
            u32 minimum = 1,
            plt::WindowEvents* events = nullptr,
            plt::InputSink* input = nullptr
        )
            : controlFd(controlFd_)
            , owner(stl::ObjPool::fromMemory())
        {
            platform = plt::Platform::create(*owner);
            window = platform->createWindow(
                *owner,
                {
                    .appId = stl::StringView(u8"plt.integration"),
                    .title = stl::StringView(u8"plt integration"),
                    .width = width,
                    .height = 600,
                    .minimumWidth = minimum,
                    .minimumHeight = minimum,
                    .input = input,
                    .events = events,
                }
            );
            window->show();
        }

        int controlFd;
        stl::ObjPool::Ref owner;
        plt::Platform* platform = nullptr;
        plt::Window* window = nullptr;
    };

    bool deferredClipboard(int fd) {
        Client client(fd);
        client.window->writeClipboard(stl::StringView(u8"clipboard"));
        const Reply before = command(fd, Command::PointerEnter);
        pump(*client.platform);
        const Reply after = command(fd, Command::QuerySelection);
        if (before.count != 0 || after.count != 1) {
            fprintf(
                stderr,
                "deferred clipboard: selection count before=%u after=%u, expected 0/1\n",
                before.count,
                after.count
            );
            return false;
        }
        return true;
    }

    bool fractionalRounding(int fd) {
        Client client(fd, 802);
        command(fd, Command::PreferredScale);
        pump(*client.platform);
        const u32 width = client.window->info().width;
        if (width != 1003) {
            fprintf(
                stderr,
                "fractional rounding: width=%u, expected 1003\n",
                width
            );
            return false;
        }
        return true;
    }

    bool minimumAfterScale(int fd) {
        Client client(fd, 800, 500);
        const Reply before = command(fd, Command::QueryMinimum);
        command(fd, Command::PreferredScale);
        Reply after;
        for (unsigned attempt = 0; attempt != 100; ++attempt) {
            pump(*client.platform);
            after = command(fd, Command::QueryMinimum);
            if (after.count > before.count) {
                break;
            }
        }
        if (before.first != 500 || after.count <= before.count
            || after.first != 400) {
            fprintf(
                stderr,
                "minimum after scale: before=%dx%d count=%u, after=%dx%d count=%u; expected second 400x400\n",
                before.first,
                before.second,
                before.count,
                after.first,
                after.second,
                after.count
            );
            return false;
        }
        return true;
    }

    struct ReadSink final: plt::ClipboardRead {
        bool data(StringView chunk) override {
            content.append(chunk.data(), chunk.length());
            return true;
        }

        void done(bool success_) override {
            success = success_;
            complete = true;
        }

        Buffer content;
        bool complete = false;
        bool success = false;
    };

    struct RejectSink final: plt::ClipboardRead {
        bool data(StringView) override {
            ++dataCount;
            return false;
        }

        void done(bool success_) override {
            ++doneCount;
            success = success_;
        }

        u32 dataCount = 0;
        u32 doneCount = 0;
        bool success = true;
    };

    struct EventSink final: plt::WindowEvents {
        void close() override {
            ++closeCount;
        }

        void resized(const plt::WindowInfo& info) override {
            ++resizeCount;
            lastInfo = info;
        }

        void redraw() override {
            ++redrawCount;
        }

        void frame() override {
            ++frameCount;
        }

        plt::WindowInfo lastInfo;
        u32 closeCount = 0;
        u32 resizeCount = 0;
        u32 redrawCount = 0;
        u32 frameCount = 0;
    };

    struct InputRecorder final: plt::InputSink {
        void key(const plt::KeyInput& input) override {
            lastKey = input;
            if (input.action == plt::InputAction::Press) {
                pressedKey = input;
                ++pressCount;
            } else if (input.action == plt::InputAction::Repeat) {
                ++repeatCount;
            } else {
                ++releaseCount;
            }
        }

        void text(const plt::TextInput& input) override {
            lastText = input;
            ++textCount;
        }

        void pointerMotion(const plt::PointerMotionInput& input) override {
            lastMotion = input;
            ++motionCount;
        }

        void pointerButton(const plt::PointerButtonInput& input) override {
            lastButton = input;
            if (input.pressed) {
                ++buttonPressCount;
            } else {
                ++buttonReleaseCount;
            }
        }

        void scroll(const plt::ScrollInput& input) override {
            lastScroll = input;
            ++scrollCount;
        }

        void focus(bool focused) override {
            if (focused) {
                ++focusCount;
            } else {
                ++blurCount;
            }
        }

        void pointerPresence(bool present) override {
            if (present) {
                ++pointerEnterCount;
            } else {
                ++pointerLeaveCount;
            }
        }

        void flush() override {
            ++flushCount;
        }

        plt::KeyInput pressedKey;
        plt::KeyInput lastKey;
        plt::TextInput lastText;
        plt::PointerMotionInput lastMotion;
        plt::PointerButtonInput lastButton;
        plt::ScrollInput lastScroll;
        u32 pressCount = 0;
        u32 repeatCount = 0;
        u32 releaseCount = 0;
        u32 textCount = 0;
        u32 motionCount = 0;
        u32 buttonPressCount = 0;
        u32 buttonReleaseCount = 0;
        u32 scrollCount = 0;
        u32 focusCount = 0;
        u32 blurCount = 0;
        u32 pointerEnterCount = 0;
        u32 pointerLeaveCount = 0;
        u32 flushCount = 0;
    };

    bool windowApi(int fd) {
        EventSink events;
        Client client(fd, 800, 1, &events);
        client.window->show();

        const plt::RenderContext render = client.window->renderContext();
        const plt::WindowInfo initial = client.window->info();
        if (render.backend != plt::RenderBackend::Wayland
            || render.connection == nullptr || render.window == nullptr
            || initial.width != 800 || initial.height != 600
            || initial.screenPixelWidth != 1920
            || initial.screenPixelHeight != 1080
            || initial.contentScale != 1
            || initial.focused || initial.maximized || initial.fullscreen
            || initial.tiled || initial.iconified
            || events.resizeCount == 0 || events.redrawCount == 0) {
            fprintf(stderr, "window API: invalid initial state or render context\n");
            return false;
        }

        const u32 redraws = events.redrawCount;
        client.window->requestRedraw();
        client.window->requestClose();
        client.window->requestClose();
        if (events.redrawCount != redraws + 1 || events.closeCount != 1) {
            fprintf(stderr, "window API: direct callbacks were not idempotent\n");
            return false;
        }

        client.window->setTitle(StringView(u8"updated title"));
        client.window->setMinimumSize(320, 240);
        client.window->setResizeUnit(10, 20, 3, 7);
        command(fd, Command::PointerEnter);
        pump(*client.platform);
        client.window->move(11, 22);
        client.window->setMaximized(true);
        client.window->setMaximized(false);
        client.window->setFullscreen(true);
        client.window->setFullscreen(false);
        client.window->restore();
        client.window->iconify();
        client.window->requestAttention();
        pump(*client.platform);
        if (command(fd, Command::QueryActivation).count != 1) {
            fprintf(stderr, "window API: requestAttention did not activate\n");
            return false;
        }
        client.window->focus();
        pump(*client.platform);
        if (command(fd, Command::QueryActivation).count != 2) {
            fprintf(stderr, "window API: focus did not activate\n");
            return false;
        }

        const u32 expectedRequests =
            UpdatedTitle | InitialAppId | Move | Maximize | Unmaximize
            | Fullscreen | Unfullscreen | Minimize;
        const Reply requests = command(fd, Command::QueryWindowRequests);
        const Reply minimum = command(fd, Command::QueryMinimum);
        if ((requests.count & expectedRequests) != expectedRequests
            || minimum.first != 320 || minimum.second != 240) {
            fprintf(
                stderr,
                "window API: request mask=%x minimum=%dx%d\n",
                requests.count,
                minimum.first,
                minimum.second
            );
            return false;
        }

        command(fd, Command::ConfigureWindowState);
        pump(*client.platform);
        const plt::WindowInfo state = client.window->info();
        if (state.width != 900 || state.height != 700 || !state.focused
            || !state.maximized || !state.fullscreen || !state.tiled) {
            fprintf(stderr, "window API: configured state was not exposed\n");
            return false;
        }

        command(fd, Command::ConfigureWindowResize);
        pump(*client.platform);
        const plt::WindowInfo resized = client.window->info();
        if (resized.width != 813 || resized.height != 627
            || resized.focused || resized.maximized
            || resized.fullscreen || resized.tiled) {
            fprintf(
                stderr,
                "window API: snapped size=%ux%u expected 813x627\n",
                resized.width,
                resized.height
            );
            return false;
        }

        client.window->resize(640, 480);
        pump(*client.platform);
        const Reply geometry = command(fd, Command::QueryWindowGeometry);
        if (geometry.first != 640 || geometry.second != 480
            || client.window->info().width != 640
            || client.window->info().height != 480) {
            fprintf(stderr, "window API: explicit resize failed\n");
            return false;
        }

        command(fd, Command::CloseWindow);
        pump(*client.platform);
        if (events.closeCount != 1) {
            fprintf(stderr, "window API: duplicate close callback\n");
            return false;
        }
        return true;
    }

    bool frameApi(int fd) {
        EventSink events;
        Client client(fd, 800, 1, &events);
        if (!client.window->requestFrame() || !client.window->requestFrame()) {
            fprintf(stderr, "frame API: requestFrame failed\n");
            return false;
        }
        pump(*client.platform);
        Reply frames = command(fd, Command::QueryFrames);
        if (frames.count != 1 || frames.first != 1) {
            fprintf(stderr, "frame API: duplicate request was sent\n");
            return false;
        }

        client.window->cancelFrame();
        command(fd, Command::CompleteFrames);
        pump(*client.platform);
        if (events.frameCount != 0) {
            fprintf(stderr, "frame API: cancelled callback was delivered\n");
            return false;
        }

        if (!client.window->requestFrame()) {
            fprintf(stderr, "frame API: second request failed\n");
            return false;
        }
        pump(*client.platform);
        frames = command(fd, Command::QueryFrames);
        if (frames.count != 2 || frames.first != 1) {
            fprintf(stderr, "frame API: second request was not sent\n");
            return false;
        }
        command(fd, Command::CompleteFrames);
        pump(*client.platform);
        client.window->cancelFrame();
        if (events.frameCount != 1) {
            fprintf(stderr, "frame API: completion was not delivered\n");
            return false;
        }
        return true;
    }

    bool pointerInput(int fd) {
        InputRecorder input;
        Client client(fd, 800, 1, nullptr, &input);
        command(fd, Command::PointerEnter);
        pump(*client.platform);
        client.window->pointerIcon(plt::PointerIcon::Link);
        pump(*client.platform);
        const Reply cursor = command(fd, Command::QueryCursor);
        if (cursor.count < 2
            || cursor.first != WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER) {
            fprintf(stderr, "pointer input: cursor shape was not updated\n");
            return false;
        }

        command(fd, Command::PointerSequence);
        pump(*client.platform);
        client.window->pointerIcon(plt::PointerIcon::Text);
        if (input.motionCount != 3
            || input.lastMotion.pixelX != 30
            || input.lastMotion.pixelY != 40
            || input.buttonPressCount != 1
            || input.buttonReleaseCount != 1
            || input.lastButton.button != plt::PointerButton::Primary
            || input.lastButton.pressed
            || input.scrollCount != 1
            || input.lastScroll.x != -2
            || input.lastScroll.y != 3
            || input.lastScroll.pixelX != 30
            || input.lastScroll.pixelY != 40
            || input.pointerEnterCount != 2
            || input.pointerLeaveCount != 1
            || input.flushCount != 2) {
            fprintf(stderr, "pointer input: event translation mismatch\n");
            return false;
        }
        return true;
    }

    bool keyboardInput(int fd) {
        InputRecorder input;
        Client client(fd, 800, 1, nullptr, &input);
        command(fd, Command::KeyboardEnter);
        pump(*client.platform);
        if (input.focusCount != 1 || !client.window->info().focused) {
            fprintf(stderr, "keyboard input: focus enter failed\n");
            return false;
        }

        command(fd, Command::KeyboardPress);
        pump(*client.platform);
        for (u32 attempt = 0; attempt != 20 && input.repeatCount == 0; ++attempt) {
            pump(*client.platform);
        }
        if (input.pressCount != 1 || input.repeatCount == 0
            || input.textCount == 0
            || input.pressedKey.key != plt::InputKey::Printable
            || input.pressedKey.layoutCodepoint != 'a'
            || input.pressedKey.baseCodepoint != 'a'
            || input.lastText.codepoint != 'a') {
            fprintf(stderr, "keyboard input: key/text/repeat translation failed\n");
            return false;
        }

        command(fd, Command::KeyboardRelease);
        pump(*client.platform);
        const u32 repeats = input.repeatCount;
        for (u32 attempt = 0; attempt != 5; ++attempt) {
            pump(*client.platform);
        }
        if (input.releaseCount != 1 || input.repeatCount != repeats) {
            fprintf(stderr, "keyboard input: release did not stop repeat\n");
            return false;
        }

        command(fd, Command::KeyboardLeave);
        pump(*client.platform);
        if (input.blurCount != 1 || client.window->info().focused) {
            fprintf(stderr, "keyboard input: focus leave failed\n");
            return false;
        }
        return true;
    }

    bool localSelections(int fd) {
        Client client(fd);
        command(fd, Command::PointerEnter);
        pump(*client.platform);

        client.window->writePrimary(StringView(u8"primary content"));
        client.window->writeClipboard(StringView(u8"clipboard content"));
        pump(*client.platform);
        if (command(fd, Command::QueryPrimarySelection).count != 1
            || command(fd, Command::QuerySelection).count != 1) {
            fprintf(stderr, "local selections: ownership was not published\n");
            return false;
        }

        ReadSink primary;
        ReadSink clipboard;
        client.window->readPrimary(primary);
        client.window->readClipboard(clipboard);
        if (primary.complete || clipboard.complete) {
            fprintf(stderr, "local selections: callback was synchronous\n");
            return false;
        }
        pump(*client.platform);
        if (!primary.complete || !primary.success
            || StringView(primary.content) != StringView(u8"primary content")
            || !clipboard.complete || !clipboard.success
            || StringView(clipboard.content) != StringView(u8"clipboard content")) {
            fprintf(stderr, "local selections: contents mismatch\n");
            return false;
        }

        ReadSink cancelled;
        client.window->readPrimary(cancelled);
        client.window->cancelClipboardRead(cancelled);
        pump(*client.platform);
        if (cancelled.complete || !cancelled.content.empty()) {
            fprintf(stderr, "local selections: cancelled callback was delivered\n");
            return false;
        }
        return true;
    }

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

    bool rejectedSelection(int fd) {
        Client client(fd);
        command(fd, Command::OfferSelection);
        pump(*client.platform);
        RejectSink read;
        client.window->readClipboard(read);
        if (command(fd, Command::ReleaseRead).count != 1) {
            fprintf(stderr, "rejected selection: no transfer fd\n");
            return false;
        }
        pump(*client.platform);
        if (read.dataCount != 1 || read.doneCount != 1 || read.success) {
            fprintf(stderr, "rejected selection: cancellation contract failed\n");
            return false;
        }
        return true;
    }

    struct FdCallback final: plt::PollCallback {
        explicit FdCallback(plt::Platform& platform_): platform(platform_) {}

        void ready(PollFD event_) override {
            event = event_;
            called = true;
            u8 byte;
            read(event.fd, &byte, 1);
            platform.stop();
        }

        plt::Platform& platform;
        PollFD event{};
        bool called = false;
    };

    struct DeadlineCallback final: plt::TimerCallback {
        explicit DeadlineCallback(plt::Platform& platform_): platform(platform_) {}

        void ready() override {
            called = true;
            platform.stop();
        }

        plt::Platform& platform;
        bool called = false;
    };

    bool pollerApi(int fd) {
        Client client(fd);
        int pipes[2];
        if (pipe2(pipes, O_CLOEXEC | O_NONBLOCK) != 0) {
            perror("pipe2");
            return false;
        }

        FdCallback ready(*client.platform);
        client.platform->poller()->arm(
            {.fd = pipes[0], .flags = PollFlag::In},
            ready
        );
        u8 byte = 1;
        if (write(pipes[1], &byte, 1) != 1) {
            close(pipes[0]);
            close(pipes[1]);
            return false;
        }
        client.platform->run();
        if (!ready.called || !(ready.event.flags & PollFlag::In)) {
            fprintf(stderr, "poller API: fd callback failed\n");
            close(pipes[0]);
            close(pipes[1]);
            return false;
        }

        FdCallback disarmed(*client.platform);
        client.platform->poller()->arm(
            {.fd = pipes[0], .flags = PollFlag::In},
            disarmed
        );
        client.platform->poller()->disarm(pipes[0]);
        write(pipes[1], &byte, 1);
        pump(*client.platform);
        read(pipes[0], &byte, 1);
        if (disarmed.called) {
            fprintf(stderr, "poller API: disarm failed\n");
            close(pipes[0]);
            close(pipes[1]);
            return false;
        }

        DeadlineCallback cancelled(*client.platform);
        client.platform->poller()->timeout(0, cancelled);
        client.platform->poller()->cancel(cancelled);
        pump(*client.platform);
        if (cancelled.called) {
            fprintf(stderr, "poller API: timer cancel failed\n");
            close(pipes[0]);
            close(pipes[1]);
            return false;
        }

        DeadlineCallback deadline(*client.platform);
        client.platform->poller()->deadline(monotonicNowUs(), deadline);
        client.platform->run();
        close(pipes[0]);
        close(pipes[1]);
        if (!deadline.called) {
            fprintf(stderr, "poller API: deadline failed\n");
            return false;
        }
        return true;
    }

    bool asynchronousRead(int fd) {
        Client client(fd);
        if (command(fd, Command::OfferSelection).count != 1) {
            fprintf(stderr, "async read: data device was not ready\n");
            return false;
        }
        pump(*client.platform);

        ReadSink read;
        client.window->readClipboard(read);
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
            || StringView(read.content) != StringView(u8"hermetic Wayland clipboard")) {
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

    bool asynchronousPrimary(int fd) {
        Client client(fd);
        if (command(fd, Command::OfferPrimarySelection).count != 1) {
            fprintf(stderr, "async primary: primary device was not ready\n");
            return false;
        }
        pump(*client.platform);

        ReadSink read;
        client.window->readPrimary(read);
        if (command(fd, Command::ReleaseRead).count != 1) {
            fprintf(stderr, "async primary: no read transfer fd\n");
            return false;
        }
        for (u32 attempt = 0; attempt != 10 && !read.complete; ++attempt) {
            pump(*client.platform);
        }
        if (!read.complete || !read.success
            || StringView(read.content)
                != StringView(u8"hermetic Wayland clipboard")) {
            fprintf(stderr, "async primary: read failed\n");
            return false;
        }

        command(fd, Command::PointerEnter);
        pump(*client.platform);
        client.window->writePrimary(StringView(u8"primary source"));
        pump(*client.platform);
        if (command(fd, Command::RequestPrimarySourceData).count != 1) {
            fprintf(stderr, "async primary: source was not published\n");
            return false;
        }
        pump(*client.platform);
        if (command(fd, Command::ReleaseWrite).count != 1) {
            fprintf(stderr, "async primary: source callback blocked\n");
            return false;
        }
        Reply state;
        for (u32 attempt = 0; attempt != 20; ++attempt) {
            pump(*client.platform);
            state = command(fd, Command::QueryWrite);
            if (state.first != 0) {
                break;
            }
        }
        if (state.count != sizeof("primary source") - 1 || state.first == 0) {
            fprintf(stderr, "async primary: source contents mismatch\n");
            return false;
        }
        return true;
    }

    bool cancelAsynchronousRead(int fd) {
        Client client(fd);
        command(fd, Command::OfferSelection);
        pump(*client.platform);

        ReadSink read;
        client.window->readClipboard(read);
        client.window->cancelClipboardRead(read);
        const bool completeAfterCancel = read.complete;
        const bool successAfterCancel = read.success;
        const size_t bytesAfterCancel = read.content.length();
        if (command(fd, Command::ReleaseRead).count != 1) {
            fprintf(stderr, "cancel read: no transfer fd was available\n");
            return false;
        }
        pump(*client.platform);
        if (read.complete != completeAfterCancel
            || read.success != successAfterCancel
            || read.content.length() != bytesAfterCancel) {
            fprintf(
                stderr,
                "cancel read: callback arrived after cancel returned\n"
            );
            return false;
        }
        return true;
    }

    bool asynchronousWrite(int fd) {
        static constexpr size_t contentSize = 2 * 1024 * 1024;
        Buffer content = repeated(contentSize, 'w');
        Client client(fd);
        client.window->writeClipboard(StringView(content));
        command(fd, Command::PointerEnter);
        pump(*client.platform);
        if (command(fd, Command::RequestSourceData).count != 1) {
            fprintf(stderr, "async write: selection source was not ready\n");
            return false;
        }

        // The server deliberately does not drain the pipe. A synchronous
        // source callback blocks here after the pipe buffer fills.
        pump(*client.platform);
        if (command(fd, Command::ReleaseWrite).count != 1) {
            fprintf(stderr, "async write: source send did not return\n");
            return false;
        }
        Reply state;
        for (unsigned attempt = 0; attempt != 100; ++attempt) {
            pump(*client.platform);
            state = command(fd, Command::QueryWrite);
            if (state.first != 0) {
                break;
            }
        }
        if (state.count != contentSize || state.first == 0) {
            fprintf(
                stderr,
                "async write: bytes=%u complete=%d, expected %zu/1\n",
                state.count,
                state.first,
                contentSize
            );
            return false;
        }
        return true;
    }

    bool brokenClipboardConsumer(int fd) {
        Client client(fd);
        client.window->writeClipboard(stl::StringView(u8"broken consumer"));
        command(fd, Command::PointerEnter);
        pump(*client.platform);
        if (command(fd, Command::RequestBrokenSourceData).count != 1) {
            fprintf(
                stderr,
                "broken clipboard consumer: selection source was not ready\n"
            );
            return false;
        }

        // The compositor has already closed the pipe's read end. Dispatching
        // wl_data_source.send must handle EPIPE without terminating the client.
        pump(*client.platform);
        command(fd, Command::QuerySelection);
        return true;
    }

    struct StopOnClose final: plt::WindowEvents {
        explicit StopOnClose(plt::Platform*& platform_): platform(platform_) {}

        void close() override {
            closed = true;
            platform->stop();
        }

        void resized(const plt::WindowInfo&) override {}
        void redraw() override {}
        void frame() override {}

        plt::Platform*& platform;
        bool closed = false;
    };

    bool flushBackpressure(int fd) {
        plt::Platform* platform = nullptr;
        StopOnClose events(platform);
        Client client(fd, 800, 1, &events);
        platform = client.platform;
        Buffer title = repeated(4000, 't');
        for (unsigned index = 0; index != 2048; ++index) {
            static_cast<u8*>(title.mutData())[0] =
                static_cast<u8>('a' + index % 26);
            client.window->setTitle(StringView(title));
        }
        command(fd, Command::AwaitTitles);

        // There is no timer: completion requires POLLOUT after the server
        // drains the initially full Wayland socket. The final title request
        // makes the server send xdg_toplevel.close, which stops the loop.
        client.platform->run();
        if (!events.closed) {
            fprintf(stderr, "flush backpressure: close was not delivered\n");
            return false;
        }
        return true;
    }

    using Scenario = bool (*)(int);

    bool runScenario(const char* name, Scenario scenario) {
        Server server;
        int wayland[2];
        int control[2];
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, wayland) != 0
            || socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, control) != 0) {
            perror("socketpair");
            return false;
        }
        const pid_t child = fork();
        if (child < 0) {
            perror("fork");
            return false;
        }
        if (child == 0) {
            signal(SIGPIPE, SIG_DFL);
            close(wayland[0]);
            close(control[0]);
            char fd[32];
            snprintf(fd, sizeof(fd), "%d", wayland[1]);
            setenv("WAYLAND_SOCKET", fd, 1);
            const bool success = scenario(control[1]);
            command(control[1], Command::Quit);
            _exit(success ? 0 : 1);
        }

        close(wayland[1]);
        close(control[1]);
        signal(SIGPIPE, SIG_IGN);
        server.client = wl_client_create(server.display, wayland[0]);
        const bool success = server.run(control[0], child);
        close(control[0]);
        fprintf(stderr, "%s: %s\n", name, success ? "PASS" : "FAIL");
        return success;
    }
}

int main() {
    bool success = true;
    success = runScenario("window API", windowApi) && success;
    success = runScenario("frame API", frameApi) && success;
    success = runScenario("pointer input", pointerInput) && success;
    success = runScenario("keyboard input", keyboardInput) && success;
    success = runScenario("local selections", localSelections) && success;
    success = runScenario("missing selections", missingSelections) && success;
    success = runScenario("rejected selection", rejectedSelection) && success;
    success = runScenario("poller API", pollerApi) && success;
    success = runScenario("deferred clipboard", deferredClipboard) && success;
    success = runScenario("fractional rounding", fractionalRounding) && success;
    success = runScenario("minimum after scale", minimumAfterScale) && success;
    success = runScenario("asynchronous clipboard read", asynchronousRead) && success;
    success = runScenario("asynchronous primary selection", asynchronousPrimary) && success;
    success = runScenario("cancel asynchronous clipboard read", cancelAsynchronousRead) && success;
    success = runScenario("asynchronous clipboard write", asynchronousWrite) && success;
    success = runScenario("broken clipboard consumer", brokenClipboardConsumer) && success;
    success = runScenario("Wayland flush backpressure", flushBackpressure) && success;
    return success ? 0 : 1;
}
