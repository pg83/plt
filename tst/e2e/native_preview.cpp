#include "app.h"

#include <std/dbg/insist.h>

#include <stdio.h>
#include <string.h>
#include <wayland-client.h>
#include <xdg-shell-client-protocol.h>

using namespace plt;
using namespace plt::e2e;
using namespace stl;

namespace {
    struct Editor final: public App {
        void paint(Canvas& canvas) override;
        void text(const TextInput& input) override;
        bool native = false;
        bool typed = false;
    };

    // An embedded native toolkit owns its own surface on the application's
    // display. Its wl_surface has no plt WindowImpl in its user-data slot.
    struct NativePreview {
        explicit NativePreview(Editor& app);
        ~NativePreview();
        static void global(void* data, wl_registry* registry, u32 name, const char* interface, u32 version);
        static void removed(void*, wl_registry*, u32);
        static void ping(void*, xdg_wm_base* shell, u32 serial);
        static void configure(void* data, xdg_surface* surface, u32 serial);
        static void size(void*, xdg_toplevel*, i32, i32, wl_array*);
        static void close(void* data, xdg_toplevel*);
        Editor& app;
        ObjPool::Ref owner = ObjPool::fromMemory();
        wl_registry* registry;
        wl_compositor* compositor = nullptr;
        xdg_wm_base* shell = nullptr;
        wl_surface* surface;
        xdg_surface* role;
        xdg_toplevel* toplevel;
        Surface* renderer;
    };
}

void Editor::paint(Canvas& canvas) {
    canvas.clear(native ? 0xe0b040 : typed ? 0x40a060 : 0x204060);
}

void Editor::text(const TextInput& input) {
    printf("TEXT %u\n", input.codepoint);
    typed = true;
    window->requestFrame();
}

void NativePreview::global(void* data, wl_registry* registry, u32 name, const char* interface, u32) {
    auto& preview = *static_cast<NativePreview*>(data);
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        preview.compositor = static_cast<wl_compositor*>(wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        preview.shell = static_cast<xdg_wm_base*>(wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
        static const xdg_wm_base_listener listener{ping};
        xdg_wm_base_add_listener(preview.shell, &listener, nullptr);
    }
}

void NativePreview::removed(void*, wl_registry*, u32) {
}

void NativePreview::ping(void*, xdg_wm_base* shell, u32 serial) {
    xdg_wm_base_pong(shell, serial);
}

void NativePreview::configure(void* data, xdg_surface* surface, u32 serial) {
    auto& preview = *static_cast<NativePreview*>(data);
    xdg_surface_ack_configure(surface, serial);
    preview.app.native = true;
    STD_INSIST(paintSurface(*preview.renderer, preview.app, {.width = 300, .height = 240, .contentScale = 1}));
    preview.app.native = false;
    puts("NATIVE PREVIEW FRAME");
}

void NativePreview::size(void*, xdg_toplevel*, i32, i32, wl_array*) {
}

void NativePreview::close(void* data, xdg_toplevel*) {
    static_cast<NativePreview*>(data)->app.platform->stop();
}

NativePreview::NativePreview(Editor& app_)
    : app(app_)
{
    auto* display = static_cast<wl_display*>(app.window->renderContext().connection);
    registry = wl_display_get_registry(display);
    static const wl_registry_listener registryListener{global, removed};
    wl_registry_add_listener(registry, &registryListener, this);
    STD_INSIST(wl_display_roundtrip(display) >= 0 && compositor != nullptr && shell != nullptr);
    surface = wl_compositor_create_surface(compositor);
    STD_INSIST(wl_surface_get_user_data(surface) == nullptr);
    role = xdg_wm_base_get_xdg_surface(shell, surface);
    static const xdg_surface_listener surfaceListener{configure};
    xdg_surface_add_listener(role, &surfaceListener, this);
    toplevel = xdg_surface_get_toplevel(role);
    static const xdg_toplevel_listener toplevelListener{.configure = size, .close = close};
    xdg_toplevel_add_listener(toplevel, &toplevelListener, this);
    xdg_toplevel_set_app_id(toplevel, "plt-native-preview");
    xdg_toplevel_set_title(toplevel, "Embedded native document preview");
    renderer = createSurface(*owner, {.backend = RenderBackend::Wayland, .connection = display, .window = surface});
    wl_surface_commit(surface);
}

NativePreview::~NativePreview() {
    owner = ObjPool::fromMemory();
    xdg_toplevel_destroy(toplevel);
    xdg_surface_destroy(role);
    wl_surface_destroy(surface);
    xdg_wm_base_destroy(shell);
    wl_compositor_destroy(compositor);
    wl_registry_destroy(registry);
}

int main() {
    Editor editor;
    editor.open("plt-native-editor");
    NativePreview preview(editor);
    editor.run();
}
