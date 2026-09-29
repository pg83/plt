#include "app.h"
#include "drop.h"

#include <std/ios/input.h>
#include <std/dbg/insist.h>
#include <std/lib/buffer.h>
#include <std/lib/vector.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>

using namespace plt;
using namespace plt::e2e;
using namespace stl;

namespace {
    struct Source final: public App {
        explicit Source(Platform* shared);
        ~Source();
        void paint(Canvas& canvas) override;
        void connect();
        void drag(u32 serial);
        wl_display* display = nullptr;
        wl_registry* registry = nullptr;
        wl_seat* seat = nullptr;
        wl_pointer* pointer = nullptr;
        wl_data_device_manager* manager = nullptr;
        wl_data_device* device = nullptr;
        wl_data_source* source = nullptr;
        Vector<wl_data_offer*> offers;
        bool inside = false;
    };

    struct Target final: public App, public DropTarget {
        explicit Target(Platform* shared);
        void paint(Canvas& canvas) override;
        DropReply dragOver(const DropOffer& offer, i32 x, i32 y) override;
        void dragLeft() override;
        void dropped(Drop& drop) override;
        u32 color = 0x204060;
    };

    static void global(void* data, wl_registry* registry, u32 name, const char* interface, u32) {
        auto& app = *(Source*)data;
        if (strcmp(interface, wl_seat_interface.name) == 0) {
            app.seat = (wl_seat*)wl_registry_bind(registry, name, &wl_seat_interface, 5);
        } else if (strcmp(interface, wl_data_device_manager_interface.name) == 0) {
            app.manager = (wl_data_device_manager*)wl_registry_bind(registry, name, &wl_data_device_manager_interface, 3);
        }
    }

    static void removed(void*, wl_registry*, u32) {
    }

    static void enter(void* data, wl_pointer*, u32, wl_surface* surface, wl_fixed_t, wl_fixed_t) {
        auto& app = *(Source*)data;
        app.inside = surface == app.window->renderContext().window;
    }

    static void leave(void* data, wl_pointer*, u32, wl_surface*) {
        ((Source*)data)->inside = false;
    }

    static void motion(void*, wl_pointer*, u32, wl_fixed_t, wl_fixed_t) {
    }

    static void button(void* data, wl_pointer*, u32 serial, u32, u32, u32 state) {
        auto& app = *(Source*)data;
        if (app.inside && state == WL_POINTER_BUTTON_STATE_PRESSED) {
            app.drag(serial);
        }
    }

    static void axis(void*, wl_pointer*, u32, u32, wl_fixed_t) {
    }

    static void frame(void*, wl_pointer*) {
    }

    static void axisSource(void*, wl_pointer*, u32) {
    }

    static void axisStop(void*, wl_pointer*, u32, u32) {
    }

    static void discrete(void*, wl_pointer*, u32, i32) {
    }

    static const wl_pointer_listener pointerListener = {
        .enter = enter,
        .leave = leave,
        .motion = motion,
        .button = button,
        .axis = axis,
        .frame = frame,
        .axis_source = axisSource,
        .axis_stop = axisStop,
        .axis_discrete = discrete,
    };

    static void offered(void*, wl_data_offer*, const char*) {
    }

    static void offerActions(void*, wl_data_offer*, u32) {
    }

    static void offerAction(void*, wl_data_offer*, u32) {
    }

    static const wl_data_offer_listener offerListener{offered, offerActions, offerAction};

    static void dataOffer(void* data, wl_data_device*, wl_data_offer* offer) {
        ((Source*)data)->offers.pushBack(offer);
        wl_data_offer_add_listener(offer, &offerListener, nullptr);
    }

    static void deviceEnter(void*, wl_data_device*, u32, wl_surface*, wl_fixed_t, wl_fixed_t, wl_data_offer*) {
    }

    static void deviceLeave(void*, wl_data_device*) {
    }

    static void deviceMotion(void*, wl_data_device*, u32, wl_fixed_t, wl_fixed_t) {
    }

    static void deviceDrop(void*, wl_data_device*) {
    }

    static void selection(void*, wl_data_device*, wl_data_offer*) {
    }

    static const wl_data_device_listener deviceListener{dataOffer, deviceEnter, deviceLeave, deviceMotion, deviceDrop, selection};

    static void sourceTarget(void*, wl_data_source*, const char*) {
    }

    static void send(void*, wl_data_source*, const char* mime, int fd) {
        const char* payload = setting("PLT_DROP_PAYLOAD", "dragged document");
        (void)!write(fd, payload, strlen(payload));
        ::close(fd);
        printf("SENT %s\n", mime);
    }

    static void cancelled(void*, wl_data_source*) {
        puts("SOURCE CANCELLED");
    }

    static void performed(void*, wl_data_source*) {
        puts("SOURCE RELEASED");
    }

    static void finished(void*, wl_data_source*) {
        puts("SOURCE FINISHED");
    }

    static void action(void*, wl_data_source*, u32 value) {
        printf("ACTION %u\n", value);
    }

    static const wl_data_source_listener sourceListener{sourceTarget, send, cancelled, performed, finished, action};
}

Source::Source(Platform* shared)
    : App(shared)
{
}

Source::~Source() {
    for (wl_data_offer* offer : offers) {
        wl_data_offer_destroy(offer);
    }
    if (source != nullptr) {
        wl_data_source_destroy(source);
    }
    wl_data_device_release(device);
    wl_pointer_release(pointer);
    wl_seat_release(seat);
    wl_data_device_manager_destroy(manager);
    wl_registry_destroy(registry);
}

void Source::connect() {
    display = (wl_display*)window->renderContext().connection;
    registry = wl_display_get_registry(display);
    static const wl_registry_listener registryListener{global, removed};
    wl_registry_add_listener(registry, &registryListener, this);
    STD_INSIST(wl_display_roundtrip(display) >= 0 && seat != nullptr && manager != nullptr);
    pointer = wl_seat_get_pointer(seat);
    wl_pointer_add_listener(pointer, &pointerListener, this);
    device = wl_data_device_manager_get_data_device(manager, seat);
    wl_data_device_add_listener(device, &deviceListener, this);
    STD_INSIST(wl_display_roundtrip(display) >= 0);
    puts("SOURCE READY");
}

void Source::drag(u32 serial) {
    if (strcmp(setting("PLT_DROP_MODE", ""), "no-offer") == 0) {
        wl_data_device_start_drag(device, nullptr, (wl_surface*)window->renderContext().window, nullptr, serial);
        puts("DRAG STARTED");
        return;
    }
    source = wl_data_device_manager_create_data_source(manager);
    wl_data_source_add_listener(source, &sourceListener, this);
    wl_data_source_offer(source, "text/plain;charset=utf-8");
    wl_data_source_offer(source, "text/uri-list");
    wl_data_source_set_actions(source, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY | WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE);
    wl_data_device_start_drag(device, source, (wl_surface*)window->renderContext().window, nullptr, serial);
    puts("DRAG STARTED");
}

void Source::paint(Canvas& canvas) {
    canvas.clear(0xe0b040);
    canvas.text(20, 40, StringView(u8"Drag this document"));
}

Target::Target(Platform* shared)
    : App(shared)
{
}

void Target::paint(Canvas& canvas) {
    canvas.clear(color);
    canvas.text(20, 40, StringView(u8"Document drop target"));
}

DropReply Target::dragOver(const DropOffer& offer, i32 x, i32) {
    const char* mode = setting("PLT_DROP_MODE", "copy");
    printf("HOVER %d\n", x);
    if (strcmp(mode, "renegotiate") == 0) {
        return {x >= 180 ? StringView(u8"text/uri-list") : StringView(u8"text/plain;charset=utf-8"), x >= 280 ? DropAction::Move : DropAction::Copy};
    }
    if (strcmp(mode, "close-hover") == 0) {
        owner = ObjPool::fromMemory();
        window = nullptr;
        surface = nullptr;
        puts("TARGET CLOSED");
        return {};
    }
    for (size_t i = 0; i != offer.formats(); ++i) {
        STD_INSIST(!offer.format(i).empty());
    }
    if (strcmp(mode, "reject") == 0) {
        return {};
    }
    if (strcmp(mode, "unknown") == 0) {
        return {StringView(u8"application/unknown"), DropAction::Copy};
    }
    if (strcmp(mode, "none") == 0) {
        return {StringView(u8"text/plain;charset=utf-8"), DropAction::None};
    }
    return {StringView(u8"text/plain;charset=utf-8"), strcmp(mode, "move") == 0 ? DropAction::Move : DropAction::Copy};
}

void Target::dragLeft() {
    puts("LEFT");
}

void Target::dropped(Drop& drop) {
    STD_INSIST(drop.what()->formats() == 2);
    if (strcmp(setting("PLT_DROP_MODE", ""), "ignore") == 0) {
        puts("IGNORED");
        return;
    }
    Input* input = drop.read(StringView(u8"text/plain;charset=utf-8"));
    Buffer content;
    if (strcmp(setting("PLT_DROP_MODE", ""), "partial") == 0) {
        char byte;
        STD_INSIST(input->read(&byte, 1) == 1);
        puts("PARTIAL");
    } else {
        input->readAll(content);
        printf("DROPPED [%s]\n", content.cStr());
    }
    delete input;
    // A document import consumes the offer once, even if a decoder retries.
    input = drop.read(StringView(u8"text/plain;charset=utf-8"));
    char extra;
    STD_INSIST(input->read(&extra, 1) == 0);
    delete input;
    color = 0x40a060;
    window->requestFrame();
}

int main() {
    auto owner = ObjPool::fromMemory();
    Platform* platform = Platform::create(*owner);
    Target target(platform);
    target.open("plt-drop-target", 400, 280, strcmp(setting("PLT_DROP_MODE", ""), "no-target") == 0 ? nullptr : &target);
    Source source(target.platform);
    source.open("plt-drag-source", 300, 220);
    source.connect();
    target.run();
}
