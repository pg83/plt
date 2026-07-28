#include "platform.h"
#include "poller.h"
#include "window.h"

#include "fractional-scale-v1-server-protocol.h"
#include "viewporter-server-protocol.h"
#include "xdg-shell-server-protocol.h"

#include <std/mem/obj_pool.h>
#include <std/str/view.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <string>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

namespace {
    enum class Command : uint32_t {
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
        Quit,
    };

    struct Reply {
        uint32_t count = 0;
        int32_t first = 0;
        int32_t second = 0;
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

        wl_display* display = nullptr;
        wl_event_loop* loop = nullptr;
        wl_client* client = nullptr;
        wl_resource* pointer = nullptr;
        wl_resource* dataDevice = nullptr;
        wl_resource* dataSource = nullptr;
        wl_resource* dataOffer = nullptr;
        Surface* window = nullptr;
        wl_event_source* writeSource = nullptr;
        int readWriteFd = -1;
        int writeReadFd = -1;
        uint32_t writtenBytes = 0;
        uint32_t titleCount = 0;
        uint32_t targetTitleCount = 0;
        uint32_t serial = 1;
        uint32_t selectionCount = 0;
        int32_t minimumWidth = 0;
        int32_t minimumHeight = 0;
        uint32_t minimumCount = 0;
    };

    void destroyResource(wl_client*, wl_resource* resource) {
        wl_resource_destroy(resource);
    }

    void bindSimple(
        wl_client* client,
        void*,
        uint32_t version,
        uint32_t id,
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
        .attach = [](wl_client*, wl_resource*, wl_resource*, int32_t, int32_t) {},
        .damage = [](wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t) {},
        .frame = [](wl_client* client, wl_resource*, uint32_t id) {
            wl_resource* const callback =
                wl_resource_create(client, &wl_callback_interface, 1, id);
            wl_callback_send_done(callback, 0);
            wl_resource_destroy(callback);
        },
        .set_opaque_region = [](wl_client*, wl_resource*, wl_resource*) {},
        .set_input_region = [](wl_client*, wl_resource*, wl_resource*) {},
        .commit = surfaceCommit,
        .set_buffer_transform = [](wl_client*, wl_resource*, int32_t) {},
        .set_buffer_scale = [](wl_client*, wl_resource*, int32_t) {},
        .damage_buffer = [](wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t) {},
        .offset = [](wl_client*, wl_resource*, int32_t, int32_t) {},
        .get_release = nullptr,
    };

    const struct wl_region_interface regionImplementation{
        .destroy = destroyResource,
        .add = [](wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t) {},
        .subtract = [](wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t) {},
    };

    const struct wl_compositor_interface compositorImplementation{
        .create_surface = [](wl_client* client, wl_resource* resource, uint32_t id) {
            auto* const server =
                static_cast<Server*>(wl_resource_get_user_data(resource));
            wl_resource* const surfaceResource = wl_resource_create(
                client,
                &wl_surface_interface,
                std::min(6, wl_resource_get_version(resource)),
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
        .create_region = [](wl_client* client, wl_resource* resource, uint32_t id) {
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
        .set_cursor = [](wl_client*, wl_resource*, uint32_t, wl_resource*, int32_t, int32_t) {},
        .release = destroyResource,
    };

    const struct wl_seat_interface seatImplementation{
        .get_pointer = [](wl_client* client, wl_resource* resource, uint32_t id) {
            auto* const server =
                static_cast<Server*>(wl_resource_get_user_data(resource));
            server->pointer = wl_resource_create(
                client,
                &wl_pointer_interface,
                std::min(8, wl_resource_get_version(resource)),
                id
            );
            wl_resource_set_implementation(
                server->pointer,
                &pointerImplementation,
                server,
                nullptr
            );
        },
        .get_keyboard = nullptr,
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
        .set_actions = [](wl_client*, wl_resource*, uint32_t) {},
    };

    const struct wl_data_offer_interface dataOfferImplementation{
        .accept = [](wl_client*, wl_resource*, uint32_t, const char*) {},
        .receive = [](wl_client*, wl_resource* resource, const char*, int32_t fd) {
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
        .set_actions = [](wl_client*, wl_resource*, uint32_t, uint32_t) {},
    };

    const struct wl_data_device_interface dataDeviceImplementation{
        .start_drag = nullptr,
        .set_selection = [](wl_client*, wl_resource* resource, wl_resource*, uint32_t) {
            auto* const server =
                static_cast<Server*>(wl_resource_get_user_data(resource));
            ++server->selectionCount;
        },
        .release = destroyResource,
    };

    const struct wl_data_device_manager_interface dataManagerImplementation{
        .create_data_source =
            [](wl_client* client, wl_resource* resource, uint32_t id) {
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
            [](wl_client* client, wl_resource* resource, uint32_t id, wl_resource*) {
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

    const struct xdg_toplevel_interface toplevelImplementation{
        .destroy = destroyResource,
        .set_parent = [](wl_client*, wl_resource*, wl_resource*) {},
        .set_title = [](wl_client*, wl_resource* resource, const char*) {
            auto* const surface =
                static_cast<Surface*>(wl_resource_get_user_data(resource));
            Server& server = *surface->server;
            ++server.titleCount;
            if (server.targetTitleCount != 0
                && server.titleCount >= server.targetTitleCount) {
                xdg_toplevel_send_close(resource);
                wl_display_flush_clients(server.display);
                server.targetTitleCount = 0;
            }
        },
        .set_app_id = [](wl_client*, wl_resource*, const char*) {},
        .show_window_menu = [](wl_client*, wl_resource*, wl_resource*, uint32_t, int32_t, int32_t) {},
        .move = [](wl_client*, wl_resource*, wl_resource*, uint32_t) {},
        .resize = [](wl_client*, wl_resource*, wl_resource*, uint32_t, uint32_t) {},
        .set_max_size = [](wl_client*, wl_resource*, int32_t, int32_t) {},
        .set_min_size = [](wl_client*, wl_resource* resource, int32_t width, int32_t height) {
            auto* const surface =
                static_cast<Surface*>(wl_resource_get_user_data(resource));
            surface->server->minimumWidth = width;
            surface->server->minimumHeight = height;
            ++surface->server->minimumCount;
        },
        .set_maximized = [](wl_client*, wl_resource*) {},
        .unset_maximized = [](wl_client*, wl_resource*) {},
        .set_fullscreen = [](wl_client*, wl_resource*, wl_resource*) {},
        .unset_fullscreen = [](wl_client*, wl_resource*) {},
        .set_minimized = [](wl_client*, wl_resource*) {},
    };

    const struct xdg_surface_interface xdgSurfaceImplementation{
        .destroy = destroyResource,
        .get_toplevel = [](wl_client* client, wl_resource* resource, uint32_t id) {
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
            [](wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t) {},
        .ack_configure = [](wl_client*, wl_resource*, uint32_t) {},
    };

    const struct xdg_wm_base_interface wmBaseImplementation{
        .destroy = destroyResource,
        .create_positioner = nullptr,
        .get_xdg_surface =
            [](wl_client* client, wl_resource* resource, uint32_t id, wl_resource* wlSurface) {
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
        .pong = [](wl_client*, wl_resource*, uint32_t) {},
    };

    const struct wp_viewport_interface viewportImplementation{
        .destroy = destroyResource,
        .set_source = [](wl_client*, wl_resource*, wl_fixed_t, wl_fixed_t, wl_fixed_t, wl_fixed_t) {},
        .set_destination = [](wl_client*, wl_resource*, int32_t, int32_t) {},
    };

    const struct wp_viewporter_interface viewporterImplementation{
        .destroy = destroyResource,
        .get_viewport =
            [](wl_client* client, wl_resource*, uint32_t id, wl_resource*) {
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
            [](wl_client* client, wl_resource*, uint32_t id, wl_resource* wlSurface) {
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

    void bindCompositor(
        wl_client* client,
        void* data,
        uint32_t version,
        uint32_t id
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

    void bindSeat(wl_client* client, void* data, uint32_t version, uint32_t id) {
        wl_resource* const resource =
            wl_resource_create(client, &wl_seat_interface, version, id);
        wl_resource_set_implementation(
            resource,
            &seatImplementation,
            data,
            nullptr
        );
        wl_seat_send_capabilities(resource, WL_SEAT_CAPABILITY_POINTER);
        if (version >= WL_SEAT_NAME_SINCE_VERSION) {
            wl_seat_send_name(resource, "plt-test-seat");
        }
    }

    void bindDataManager(
        wl_client* client,
        void* data,
        uint32_t version,
        uint32_t id
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
        uint32_t version,
        uint32_t id
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
        uint32_t version,
        uint32_t id
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
        uint32_t version,
        uint32_t id
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

    void bindOutput(
        wl_client* client,
        void*,
        uint32_t version,
        uint32_t id
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
        wl_array states;
        wl_array_init(&states);
        xdg_toplevel_send_configure(surface.toplevel, 0, 0, &states);
        xdg_surface_send_configure(surface.xdgSurface, serial++);
        wl_array_release(&states);
        surface.configured = true;
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
                        [](int fd, uint32_t mask, void* data) {
                            auto* const server = static_cast<Server*>(data);
                            char buffer[16384];
                            for (;;) {
                                const ssize_t count =
                                    read(fd, buffer, sizeof(buffer));
                                if (count > 0) {
                                    server->writtenBytes +=
                                        static_cast<uint32_t>(count);
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
                targetTitleCount = static_cast<uint32_t>(reply.first = 2049);
                reply.count = titleCount;
                if (window != nullptr && window->toplevel != nullptr
                    && titleCount >= targetTitleCount) {
                    xdg_toplevel_send_close(window->toplevel);
                    wl_display_flush_clients(display);
                    targetTitleCount = 0;
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
            uint32_t width = 800,
            uint32_t minimum = 1,
            plt::WindowEvents* events = nullptr
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
            std::fprintf(
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
        const uint32_t width = client.window->info().width;
        if (width != 1003) {
            std::fprintf(
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
            std::fprintf(
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
        bool data(stl::StringView chunk) override {
            content.append(
                reinterpret_cast<const char*>(chunk.data()),
                chunk.length()
            );
            return true;
        }

        void done(bool success_) override {
            success = success_;
            complete = true;
        }

        std::string content;
        bool complete = false;
        bool success = false;
    };

    bool asynchronousRead(int fd) {
        Client client(fd);
        if (command(fd, Command::OfferSelection).count != 1) {
            std::fprintf(stderr, "async read: data device was not ready\n");
            return false;
        }
        pump(*client.platform);

        ReadSink read;
        client.window->readClipboard(read);
        const Reply released = command(fd, Command::ReleaseRead);
        if (released.count != 1) {
            std::fprintf(
                stderr,
                "async read: readClipboard returned before server received receive, but no transfer fd was available\n"
            );
            return false;
        }
        for (unsigned attempt = 0; attempt != 10 && !read.complete; ++attempt) {
            pump(*client.platform);
        }
        if (!read.complete || !read.success
            || read.content != "hermetic Wayland clipboard") {
            std::fprintf(
                stderr,
                "async read: complete=%d success=%d bytes=%zu\n",
                read.complete,
                read.success,
                read.content.size()
            );
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
        const size_t bytesAfterCancel = read.content.size();
        if (command(fd, Command::ReleaseRead).count != 1) {
            std::fprintf(stderr, "cancel read: no transfer fd was available\n");
            return false;
        }
        pump(*client.platform);
        if (read.complete != completeAfterCancel
            || read.success != successAfterCancel
            || read.content.size() != bytesAfterCancel) {
            std::fprintf(
                stderr,
                "cancel read: callback arrived after cancel returned\n"
            );
            return false;
        }
        return true;
    }

    bool asynchronousWrite(int fd) {
        static constexpr size_t contentSize = 2 * 1024 * 1024;
        std::string content(contentSize, 'w');
        Client client(fd);
        client.window->writeClipboard(stl::StringView(
            reinterpret_cast<const char8_t*>(content.data()),
            content.size()
        ));
        command(fd, Command::PointerEnter);
        pump(*client.platform);
        if (command(fd, Command::RequestSourceData).count != 1) {
            std::fprintf(stderr, "async write: selection source was not ready\n");
            return false;
        }

        // The server deliberately does not drain the pipe. A synchronous
        // source callback blocks here after the pipe buffer fills.
        pump(*client.platform);
        if (command(fd, Command::ReleaseWrite).count != 1) {
            std::fprintf(stderr, "async write: source send did not return\n");
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
            std::fprintf(
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
            std::fprintf(
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
        std::string title(4000, 't');
        for (unsigned index = 0; index != 2048; ++index) {
            title[0] = static_cast<char>('a' + index % 26);
            client.window->setTitle(stl::StringView(
                reinterpret_cast<const char8_t*>(title.data()),
                title.size()
            ));
        }
        command(fd, Command::AwaitTitles);

        // There is no timer: completion requires POLLOUT after the server
        // drains the initially full Wayland socket. The final title request
        // makes the server send xdg_toplevel.close, which stops the loop.
        client.platform->run();
        if (!events.closed) {
            std::fprintf(stderr, "flush backpressure: close was not delivered\n");
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
            std::perror("socketpair");
            return false;
        }
        const pid_t child = fork();
        if (child < 0) {
            std::perror("fork");
            return false;
        }
        if (child == 0) {
            signal(SIGPIPE, SIG_DFL);
            close(wayland[0]);
            close(control[0]);
            char fd[32];
            std::snprintf(fd, sizeof(fd), "%d", wayland[1]);
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
        std::fprintf(stderr, "%s: %s\n", name, success ? "PASS" : "FAIL");
        return success;
    }
}

int main() {
    bool success = true;
    success = runScenario("deferred clipboard", deferredClipboard) && success;
    success = runScenario("fractional rounding", fractionalRounding) && success;
    success = runScenario("minimum after scale", minimumAfterScale) && success;
    success = runScenario("asynchronous clipboard read", asynchronousRead) && success;
    success = runScenario("cancel asynchronous clipboard read", cancelAsynchronousRead) && success;
    success = runScenario("asynchronous clipboard write", asynchronousWrite) && success;
    success = runScenario("broken clipboard consumer", brokenClipboardConsumer) && success;
    success = runScenario("Wayland flush backpressure", flushBackpressure) && success;
    return success ? 0 : 1;
}
