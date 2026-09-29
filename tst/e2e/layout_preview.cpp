#include "app.h"
#include "poller.h"
#include "platform_headless.h"

#include <std/ios/input.h>
#include <std/dbg/insist.h>
#include <std/ios/output.h>
#include <std/lib/vector.h>

#include <cairo.h>
#include <stdio.h>
#include <string.h>

using namespace plt;
using namespace plt::e2e;
using namespace stl;

namespace {
    struct Document final: public FrameCallback, public WindowEvents, public TimerCallback {
        explicit Document(ObjPool& owner);
        bool frame(const WindowInfo& info) override;
        void close() override;
        void ready() override;
        void render();
        void save(const char* name);
        Platform* platform;
        WindowHeadless* window;
        bool changed = false;
    };

    struct Preview final: public App {
        void paint(Canvas& canvas) override;
        void key(const KeyInput& input) override;
        Document* document = nullptr;
    };
}

Document::Document(ObjPool& owner)
    : platform(createHeadlessPlatform(owner))
{
    window = static_cast<WindowHeadless*>(platform->createWindow(
        owner,
        {
            .width = 96,
            .height = 64,
            .events = this,
            .frame = this,
        }
    ));
    auto info = window->info();
    info.screenPixelWidth = 640;
    info.screenPixelHeight = 480;
    window->configure(info);
    window->requestTitle(StringView(u8"Export preview"));
    window->requestMinimumSize(32, 32);
    window->requestResizeUnit(8, 8, 0, 0);
    window->requestMove(0, 0);
    window->requestFocus();
    window->requestTextInputRect(0, 0, 8, 8);
    window->requestPointerIcon(PointerIcon::Crosshair);
    STD_INSIST(window->pointerIcon() == PointerIcon::Crosshair);
    STD_INSIST(window->title() == StringView(u8"Export preview"));
    STD_INSIST(!window->inLiveResize());
    // A renderer without a desktop must also tolerate an absent clipboard.
    Input* input = window->primary()->read();
    char byte;
    STD_INSIST(input->read(&byte, 1) == 0);
    delete input;
    Output* output = window->secondary()->write();
    output->write("draft", 5);
    output->finish();
    delete output;
    window->requestShow();
}

bool Document::frame(const WindowInfo& info) {
    auto* const target = static_cast<HeadlessRenderTarget*>(window->renderContext().window);
    if (target->pixels == nullptr) {
        window->requestFrame();
        return false;
    }
    for (u32 y = 0; y < info.height; ++y) {
        for (u32 x = 0; x < info.width; ++x) {
            const u32 color = x < info.width / 2 ? (changed ? 0x8040a0 : 0x204060) : 0xe0b040;
            u8* pixel = target->pixels + y * target->stride + x * 3;
            pixel[0] = color >> 16;
            pixel[1] = color >> 8;
            pixel[2] = color;
        }
    }
    return true;
}

void Document::close() {
    platform->stop();
}

void Document::ready() {
    window->dispatchFrame();
    if (window->framePending()) {
        platform->poller()->defer(*this);
    } else {
        platform->stop();
    }
}

void Document::render() {
    platform->poller()->timeout(100, *this);
    platform->run();
    STD_INSIST(!window->framePending());
}

void Document::save(const char* name) {
    render();
    const auto image = window->presentedFrame();
    char path[4096];
    snprintf(path, sizeof(path), "%s/%s.ppm", setting("PLT_EXPORT_DIR", ""), name);
    FILE* const file = fopen(path, "wb");
    STD_INSIST(file != nullptr);
    fprintf(file, "P6\n%u %u\n255\n", image.width, image.height);
    STD_INSIST(fwrite(image.pixels, 1, image.length, file) == image.length);
    STD_INSIST(fclose(file) == 0);
    printf("EXPORTED %s %u %u %llu\n", name, image.width, image.height, (unsigned long long)image.generation);
}

void Preview::paint(Canvas& c) {
    c.clear(0x40a060);
    c.text(20, 35, StringView(u8"Offline layout preview"));
    const auto frame = document->window->presentedFrame();
    Vector<u32> pixels;
    pixels.zero(frame.width * frame.height);
    for (size_t i = 0; i < pixels.length(); ++i) {
        pixels.mut(i) = 0xff000000 | (frame.pixels[i * 3] << 16) | (frame.pixels[i * 3 + 1] << 8) | frame.pixels[i * 3 + 2];
    }
    cairo_surface_t* const image = cairo_image_surface_create_for_data(reinterpret_cast<unsigned char*>(pixels.mutData()), CAIRO_FORMAT_ARGB32, frame.width, frame.height, frame.width * 4);
    cairo_save(c.context);
    cairo_translate(c.context, 40, 60);
    cairo_scale(c.context, 320.0 / frame.width, 160.0 / frame.height);
    cairo_set_source_surface(c.context, image, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(c.context), CAIRO_FILTER_NEAREST);
    cairo_paint(c.context);
    cairo_restore(c.context);
    cairo_surface_destroy(image);
}

void Preview::key(const KeyInput& input) {
    App::key(input);
    if (input.action != InputAction::Press) {
        return;
    }
    const char* name = nullptr;
    switch (input.baseCodepoint) {
        case 'r': {
            document->window->requestResize(256, 192);
            name = "resized";
            break;
        }
        case 'm': {
            document->window->requestMaximized(true);
            name = "maximized";
            break;
        }
        case 'n': {
            document->window->requestMaximized(false);
            name = "restored";
            break;
        }
        case 'f': {
            document->window->requestFullscreen(true);
            name = "fullscreen";
            break;
        }
        case 'w': {
            document->window->requestFullscreen(false);
            document->window->requestIconify();
            document->window->requestRestore();
            document->window->requestAttention();
            name = "windowed";
            break;
        }
        case 'p': {
            document->changed = true;
            document->window->failNextPresentation();
            name = "retried";
            break;
        }
        case 'l': {
            document->window->requestOpenUri(StringView(u8"https://example.invalid/preview"));
            STD_INSIST(document->window->openedUri() == StringView(u8"https://example.invalid/preview"));
            STD_INSIST(document->window->openUriCount() == 1);
            document->window->setClipboards(*window->primary(), *window->secondary());
            name = "linked";
            break;
        }
        case 'x': {
            // Preview transitions retain the original document size across
            // nested maximize/fullscreen states and duplicate requests.
            document->window->requestMaximized(true);
            document->window->requestMaximized(true);
            document->window->requestFullscreen(true);
            document->window->requestFullscreen(true);
            document->window->requestFullscreen(false);
            document->window->requestMaximized(false);
            document->window->requestFullscreen(true);
            document->window->requestMaximized(true);
            document->window->requestMaximized(false);
            document->window->requestFullscreen(false);
            name = "nested";
            break;
        }
        case 'z': {
            document->window->requestResize(0, 192);
            document->window->requestResize(256, 0);
            auto info = document->window->info();
            info.contentScale = 0;
            info.maximized = true;
            document->window->configure(info);
            document->window->requestMaximized(false);
            STD_INSIST(document->window->info().contentScale == 1.0f);
            name = "restored-config";
            break;
        }
        default: {
            return;
        }
    }
    document->save(name);
    window->requestFrame();
}

int main() {
    Preview app;
    app.document = app.owner->make<Document>(*app.owner);
    app.document->save("initial");
    app.open("plt-layout-preview");
    app.run();
    app.document->window->requestClose();
    app.document->window->requestFrame();
    STD_INSIST(!app.document->window->framePending());
}
