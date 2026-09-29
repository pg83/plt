#include "virtual-pointer-client.h"

#include <stdio.h>
#include <string.h>
#include <wayland-client.h>

extern "C" {
#include "virtual-pointer-code.h"
}

namespace {
    struct Devices {
        wl_seat* seat = nullptr;
        zwlr_virtual_pointer_manager_v1* manager = nullptr;
    };

    void global(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t) {
        auto& devices = *static_cast<Devices*>(data);
        if (strcmp(interface, wl_seat_interface.name) == 0) {
            devices.seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, 1));
        } else if (strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name) == 0) {
            devices.manager = static_cast<zwlr_virtual_pointer_manager_v1*>(wl_registry_bind(registry, name, &zwlr_virtual_pointer_manager_v1_interface, 1));
        }
    }

    void removed(void*, wl_registry*, uint32_t) {
    }
}

// Keep a real virtual pointer attached while Python drives Sway's seat commands.
int main() {
    wl_display* const display = wl_display_connect(nullptr);
    if (display == nullptr) {
        return 1;
    }
    Devices devices;
    wl_registry* const registry = wl_display_get_registry(display);
    const wl_registry_listener listener{global, removed};
    wl_registry_add_listener(registry, &listener, &devices);
    if (wl_display_roundtrip(display) < 0 || devices.seat == nullptr || devices.manager == nullptr) {
        return 1;
    }
    auto* const pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(devices.manager, devices.seat);
    if (wl_display_roundtrip(display) < 0) {
        return 1;
    }
    puts("READY");
    fflush(stdout);
    while (getchar() != EOF) {
    }
    zwlr_virtual_pointer_v1_destroy(pointer);
    zwlr_virtual_pointer_manager_v1_destroy(devices.manager);
    wl_seat_destroy(devices.seat);
    wl_registry_destroy(registry);
    wl_display_flush(display);
    wl_display_disconnect(display);
}
