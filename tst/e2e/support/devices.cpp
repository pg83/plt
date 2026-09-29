#include "input-method-client.h"
#include "virtual-pointer-client.h"

#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>

extern "C" {
#include "virtual-pointer-code.h"
#include "input-method-code.h"
}

namespace {
    struct Devices {
        wl_seat* seat = nullptr;
        zwp_input_method_manager_v2* imeManager = nullptr;
        zwp_input_method_v2* ime = nullptr;
        uint32_t imeSerial = 0;
        zwlr_virtual_pointer_manager_v1* manager = nullptr;
    };

    void global(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t) {
        auto& devices = *static_cast<Devices*>(data);
        if (strcmp(interface, wl_seat_interface.name) == 0) {
            devices.seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, 1));
        } else if (strcmp(interface, zwp_input_method_manager_v2_interface.name) == 0) {
            devices.imeManager = static_cast<zwp_input_method_manager_v2*>(wl_registry_bind(registry, name, &zwp_input_method_manager_v2_interface, 1));
        } else if (strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name) == 0) {
            devices.manager = static_cast<zwlr_virtual_pointer_manager_v1*>(wl_registry_bind(registry, name, &zwlr_virtual_pointer_manager_v1_interface, 1));
        }
    }

    void removed(void*, wl_registry*, uint32_t) {
    }

    static void activated(void*, zwp_input_method_v2*) {
        puts("IME ACTIVE");
    }

    static void deactivated(void*, zwp_input_method_v2*) {
        puts("IME INACTIVE");
    }

    static void surrounding(void*, zwp_input_method_v2*, const char*, uint32_t, uint32_t) {
    }

    static void cause(void*, zwp_input_method_v2*, uint32_t) {
    }

    static void content(void*, zwp_input_method_v2*, uint32_t, uint32_t) {
    }

    static void imeDone(void* data, zwp_input_method_v2*) {
        ++static_cast<Devices*>(data)->imeSerial;
    }

    static void unavailable(void*, zwp_input_method_v2*) {
        fputs("IME unavailable\n", stderr);
        exit(1);
    }

    static const zwp_input_method_v2_listener imeListener = {
        activated,
        deactivated,
        surrounding,
        cause,
        content,
        imeDone,
        unavailable,
    };
}

// Keep a real virtual pointer attached and inject the driver's input events.
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
    if (getenv("PLT_IME") != nullptr) {
        if (devices.imeManager == nullptr) {
            return 1;
        }
        devices.ime = zwp_input_method_manager_v2_get_input_method(devices.imeManager, devices.seat);
        zwp_input_method_v2_add_listener(devices.ime, &imeListener, &devices);
        wl_display_roundtrip(display);
    }
    puts("READY");
    fflush(stdout);
    char line[8192];
    unsigned int serial = 0;
    while (fgets(line, sizeof(line), stdin) != nullptr) {
        wl_display_roundtrip(display);
        timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        const uint32_t time = now.tv_sec * 1000 + now.tv_nsec / 1000000;
        unsigned int x, y, width, height, button, pressed;
        int steps;
        unsigned int axis;
        if (sscanf(line, "move %u %u %u %u", &x, &y, &width, &height) == 4) {
            zwlr_virtual_pointer_v1_motion_absolute(pointer, time, x, y, width, height);
        } else if (sscanf(line, "button %u %u", &button, &pressed) == 2) {
            zwlr_virtual_pointer_v1_button(pointer, time, button, pressed);
        } else if (sscanf(line, "scroll %d", &steps) == 1) {
            zwlr_virtual_pointer_v1_axis_source(pointer, WL_POINTER_AXIS_SOURCE_WHEEL);
            zwlr_virtual_pointer_v1_axis_discrete(pointer, time, WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_int(steps * 10), steps);
        } else if (sscanf(line, "wheel %u %d", &axis, &steps) == 2 && axis <= 1) {
            zwlr_virtual_pointer_v1_axis_source(pointer, WL_POINTER_AXIS_SOURCE_WHEEL);
            zwlr_virtual_pointer_v1_axis_discrete(pointer, time, axis, wl_fixed_from_int(steps * 10), steps);
        } else if (sscanf(line, "smooth %u %d", &axis, &steps) == 2 && axis <= 1) {
            zwlr_virtual_pointer_v1_axis_source(pointer, WL_POINTER_AXIS_SOURCE_FINGER);
            zwlr_virtual_pointer_v1_axis(pointer, time, axis, wl_fixed_from_int(steps));
        } else if (sscanf(line, "stop %u", &axis) == 1 && axis <= 1) {
            zwlr_virtual_pointer_v1_axis_source(pointer, WL_POINTER_AXIS_SOURCE_FINGER);
            zwlr_virtual_pointer_v1_axis_stop(pointer, time, axis);
        } else if (strncmp(line, "ime-", 4) == 0 && devices.ime != nullptr) {
            char payload[4096];
            size_t size = 0;
            char* hex = strchr(line, ' ');
            if (hex == nullptr) {
                return 1;
            }
            ++hex;
            unsigned int byte;
            while (hex[0] != 0 && hex[0] != '\n' && hex[1] != 0 && sscanf(hex, "%2x", &byte) == 1) {
                payload[size++] = byte;
                hex += 2;
            }
            payload[size] = 0;
            if (strncmp(line, "ime-preedit ", 12) == 0) {
                zwp_input_method_v2_set_preedit_string(devices.ime, payload, 0, size);
            } else {
                zwp_input_method_v2_set_preedit_string(devices.ime, "", -1, -1);
                zwp_input_method_v2_commit_string(devices.ime, payload);
            }
            zwp_input_method_v2_commit(devices.ime, devices.imeSerial);
        } else {
            return 1;
        }
        zwlr_virtual_pointer_v1_frame(pointer);
        if (wl_display_roundtrip(display) < 0) {
            return 1;
        }
        printf("DONE %u\n", ++serial);
        fflush(stdout);
    }
    if (devices.ime != nullptr) {
        zwp_input_method_v2_destroy(devices.ime);
    }
    if (devices.imeManager != nullptr) {
        zwp_input_method_manager_v2_destroy(devices.imeManager);
    }
    zwlr_virtual_pointer_v1_destroy(pointer);
    zwlr_virtual_pointer_manager_v1_destroy(devices.manager);
    wl_seat_destroy(devices.seat);
    wl_registry_destroy(registry);
    wl_display_flush(display);
    wl_display_disconnect(display);
}
