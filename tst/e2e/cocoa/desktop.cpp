#include "drop.h"
#include "fiber.h"
#include "input.h"
#include "native.h"
#include "poller.h"
#include "platform.h"

#include <std/ios/input.h>
#include <std/dbg/insist.h>
#include <std/ios/output.h>
#include <std/lib/buffer.h>
#include <std/thr/runable.h>
#include <std/mem/obj_pool.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

using namespace plt;
using namespace plt::e2e;
using namespace stl;

namespace {
    struct ImportDialog final: public PollCallback {
        ImportDialog(Platform& platform, bool replace);
        ~ImportDialog();
        void ready(PollFD event) override;
        void run();
        Platform& platform;
        bool replace;
        int pipes[2];
        unsigned phase = 0;
        PollWaiter waiter;
    };

    struct Desktop final: public FrameCallback, public WindowEvents, public InputSink, public Runable, public DropTarget {
        Desktop();
        ~Desktop();
        bool frame(const WindowInfo& info) override;
        void close() override;
        void run() override;
        void command(const char* line);
        void key(const KeyInput& input) override;
        void text(const TextInput& input) override;
        void preedit(StringView text, i32 begin, i32 end) override;
        void pointerMotion(const PointerMotionInput& input) override;
        void pointerButton(const PointerButtonInput& input) override;
        void scroll(const ScrollInput& input) override;
        void focus(bool focused) override;
        void pointerPresence(bool present) override;
        void flush() override;
        DropReply dragOver(const DropOffer& offer, i32 x, i32 y) override;
        void dragLeft() override;
        void dropped(Drop& drop) override;

        ObjPool::Ref owner = ObjPool::fromMemory();
        Platform* platform;
        Window* window;
        MetalCanvas* canvas = nullptr;
        u32 color = 0x204060;
        int pipe = -1;
        unsigned dropMode = 0;
        ObjPool::Ref auxiliary = ObjPool::fromMemory();
        Window* auxiliaryWindow = nullptr;
    };
}

ImportDialog::ImportDialog(Platform& platform_, bool replace_)
    : platform(platform_)
    , replace(replace_)
{
    STD_INSIST(::pipe(pipes) == 0);
    waiter.fd = {.fd = pipes[0], .flags = PollFlag::In};
    waiter.callback = this;
}

ImportDialog::~ImportDialog() {
    platform.poller()->cancel(waiter);
    ::close(pipes[0]);
    ::close(pipes[1]);
}

void ImportDialog::ready(PollFD event) {
    STD_INSIST(event.flags & PollFlag::In);
    ++phase;
    if (phase == 1) {
        platform.poller()->arm(waiter);
        platform.run();
        STD_INSIST(phase == 2);
        if (replace) {
            // The nested dispatch removed the old native descriptor. A new
            // subscription on the same application fd gets its own source.
            platform.poller()->arm(waiter);
        } else {
            platform.stop();
        }
    } else {
        platform.stop();
    }
}

void ImportDialog::run() {
    platform.poller()->arm(waiter);
    STD_INSIST(write(pipes[1], "x", 1) == 1);
    platform.run();
    STD_INSIST(phase == (replace ? 3u : 2u));
    char byte;
    STD_INSIST(read(pipes[0], &byte, 1) == 1 && byte == 'x');
    printf("NESTED IMPORT %u\n", phase);
}

Desktop::Desktop()
    : platform(Platform::create(*owner))
{
    Buffer icon;
    if (const char* path = getenv("PLT_ICON")) {
        FILE* file = fopen(path, "rb");
        STD_INSIST(file != nullptr);
        char bytes[4096];
        size_t count;
        while ((count = fread(bytes, 1, sizeof(bytes), file)) != 0) {
            icon.append(bytes, count);
        }
        fclose(file);
    }
    window = platform->createWindow(
        *owner,
        {
            .appId = StringView(u8"plt-cocoa-desktop"),
            .title = StringView(u8"PLT desktop"),
            .width = 400,
            .height = 280,
            .minimumWidth = 160,
            .minimumHeight = 120,
            .decorations = true,
            .input = getenv("PLT_PASSIVE") ? nullptr : createFiberInputSink(*owner, *platform->scheduler(), *this),
            .events = this,
            .frame = this,
            .drop = getenv("PLT_NO_DROP") ? nullptr : this,
            .icon = StringView(icon),
            .appName = getenv("PLT_PASSIVE") ? StringView() : StringView(u8"PLT E2E"),
        }
    );
    canvas = MetalCanvas::create(*owner, window->renderContext());
    canvas->command("compose");
    color = 0x204060;
    pipe = ::open(getenv("PLT_COMMAND_PIPE"), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    STD_INSIST(pipe >= 0);
    platform->scheduler()->create(*owner, *this, 128 * 1024);
    window->requestShow();
}

Desktop::~Desktop() {
    ::close(pipe);
}

bool Desktop::frame(const WindowInfo& info) {
    if (canvas == nullptr || !canvas->paint(info, color)) {
        return false;
    }
    printf("FRAME %u %u %.3f %d %d %d %06x\n", info.width, info.height, info.contentScale, info.focused, info.maximized, info.fullscreen, color);
    return true;
}

void Desktop::close() {
    puts("CLOSED");
    platform->stop();
}

void Desktop::run() {
    Buffer pending;
    for (;;) {
        platform->scheduler()->awaitReadable(pipe, 0);
        char data[512];
        const ssize_t count = read(pipe, data, sizeof(data));
        if (count <= 0) {
            continue;
        }
        for (ssize_t i = 0; i < count; ++i) {
            if (data[i] == '\n') {
                command(pending.cStr());
                pending.reset();
            } else {
                pending.append(data + i, 1);
            }
        }
    }
}

void Desktop::command(const char* line) {
    if (strcmp(line, "describe") == 0) {
        canvas->describe();
    } else if (strcmp(line, "resize") == 0) {
        window->requestMinimumSize(200, 160);
        window->requestResizeUnit(8, 16, 4, 6);
        window->requestResize(640, 360);
    } else if (strcmp(line, "resize-free") == 0) {
        window->requestResizeUnit(1, 1, 0, 0);
    } else if (strcmp(line, "resize-base") == 0) {
        window->requestResizeUnit(8, 16, 2000, 2000);
    } else if (strcmp(line, "open-document") == 0) {
        window->requestOpenUri(StringView(u8"\xff", 1));
        window->requestOpenUri(StringView(u8"http://["));
        Buffer uri;
        uri.append("file://", 7);
        const char* path = getenv("PLT_DROP_FILE");
        uri.append(path, strlen(path));
        window->requestOpenUri(StringView(uri));
    } else if (strcmp(line, "title") == 0) {
        window->requestTitle(StringView(u8"Edited document"));
        window->requestAttention();
    } else if (strcmp(line, "move") == 0) {
        window->requestMove(80, 100);
    } else if (strcmp(line, "minimize") == 0) {
        window->requestIconify();
    } else if (strcmp(line, "restore") == 0) {
        window->requestRestore();
        window->requestFocus();
    } else if (strcmp(line, "maximize") == 0) {
        window->requestMaximized(true);
    } else if (strcmp(line, "unmaximize") == 0) {
        window->requestMaximized(false);
    } else if (strcmp(line, "fullscreen") == 0) {
        window->requestFullscreen(true);
    } else if (strcmp(line, "unfullscreen") == 0) {
        window->requestFullscreen(false);
    } else if (strncmp(line, "cursor ", 7) == 0) {
        window->requestPointerIcon(static_cast<PointerIcon>(atoi(line + 7)));
    } else if (strncmp(line, "dropmode ", 9) == 0) {
        dropMode = atoi(line + 9);
    } else if (strcmp(line, "import-dialog") == 0) {
        ImportDialog first(*platform, false);
        first.run();
        ImportDialog replacement(*platform, true);
        replacement.run();
    } else if (strcmp(line, "caret") == 0) {
        window->requestTextInputRect(20, 40, 8, 18);
    } else if (strcmp(line, "copy") == 0 || strcmp(line, "primary") == 0) {
        Clipboard* clipboard = line[0] == 'c' ? window->secondary() : window->primary();
        Output* output = clipboard->write();
        const StringView payload(u8"plt clipboard: Привет 🌍");
        output->write(payload.data(), payload.length());
        output->finish();
        output->finish();
        delete output;
        Input* input = clipboard->read();
        Buffer readback;
        input->readAll(readback);
        delete input;
        STD_INSIST(StringView(readback) == payload);
        color = 0xe0b040;
        printf("COPIED %s\n", readback.cStr());
    } else if (strcmp(line, "paste") == 0) {
        Input* input = window->secondary()->read();
        Buffer content;
        input->readAll(content);
        delete input;
        printf("PASTED %s\n", content.cStr());
        color = 0x40a060;
    } else if (strcmp(line, "auxiliary") == 0 || strcmp(line, "retire-queued") == 0) {
        auxiliary = ObjPool::fromMemory();
        Window* pending = platform->createWindow(*auxiliary, {.appName = StringView(u8"\xff", 1)});
        auxiliaryWindow = pending;
        canvas->holdInput(pending->renderContext());
        pending->info();
        pending->requestFrame();
        pending->requestClose();
        if (strcmp(line, "retire-queued") == 0) {
            // The display-link thread queues a main-loop block while this
            // command still owns the loop. Retire its window before delivery.
            usleep(100000);
            auxiliary = ObjPool::fromMemory();
            puts("QUEUED PREVIEW RETIRED");
        }
    } else if (strcmp(line, "auxiliary-frame") == 0) {
        auxiliaryWindow->requestFrame();
        usleep(100000);
    } else if (strcmp(line, "auxiliary-offscreen") == 0) {
        auxiliaryWindow->requestMove(100000, 100000);
        const WindowInfo info = auxiliaryWindow->info();
        STD_INSIST(info.screenPixelWidth > 0 && info.screenPixelHeight > 0);
    } else if (strcmp(line, "close-auxiliary") == 0) {
        auxiliary = ObjPool::fromMemory();
        canvas->command("detached-input");
    } else if (strcmp(line, "copy-invalid") == 0) {
        Output* output = window->secondary()->write();
        output->write("\xff", 1);
        output->finish();
        delete output;
        Input* input = window->secondary()->read();
        char byte;
        STD_INSIST(input->read(&byte, 1) == 0);
        delete input;
    } else if (strcmp(line, "abandon") == 0) {
        Output* output = window->secondary()->write();
        output->write("discard", 7);
        delete output;
    } else if (strcmp(line, "quit") == 0) {
        window->requestClose();
    } else if (canvas->command(line)) {
    } else {
        fprintf(stderr, "unknown command: %s\n", line);
        exit(1);
    }
    printf("COMMAND %s %d\n", line, window->inLiveResize());
    window->requestFrame();
}

void Desktop::key(const KeyInput& input) {
    printf("KEY %u %u %u %u\n", (u32)input.key, (u32)input.action, input.baseCodepoint, input.modifiers);
}

void Desktop::text(const TextInput& input) {
    printf("TEXT %u\n", input.codepoint);
    color = 0xc04040;
    window->requestFrame();
}

void Desktop::preedit(StringView text, i32 begin, i32 end) {
    printf("PREEDIT %zu %d %d\n", text.length(), begin, end);
}

void Desktop::pointerMotion(const PointerMotionInput& input) {
    printf("MOTION %d %d\n", input.pixelX, input.pixelY);
}

void Desktop::pointerButton(const PointerButtonInput& input) {
    printf("BUTTON %u %d\n", (u32)input.button, input.pressed);
    color = 0x8040a0;
    window->requestFrame();
}

void Desktop::scroll(const ScrollInput&) {
    puts("SCROLL");
}

void Desktop::focus(bool focused) {
    printf("FOCUS %d\n", focused);
}

void Desktop::pointerPresence(bool present) {
    printf("POINTER %d\n", present);
}

void Desktop::flush() {
}

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    Desktop desktop;
    desktop.platform->run();
    return 0;
}

DropReply Desktop::dragOver(const DropOffer& offer, i32 x, i32 y) {
    printf("DRAG %zu %d %d\n", offer.formats(), x, y);
    if (dropMode == 2) {
        return {};
    }
    if (dropMode == 3) {
        return {StringView(u8"application/unknown"), DropAction::Copy};
    }
    return {offer.formats() ? offer.format(offer.formats() - 1) : StringView(), dropMode == 4 ? DropAction::None : dropMode == 1 ? DropAction::Move : DropAction::Copy};
}

void Desktop::dragLeft() {
    puts("DRAG LEFT");
}

void Desktop::dropped(Drop& drop) {
    DropOffer* offer = drop.what();
    STD_INSIST(offer->formats() > 0);
    if (dropMode == 6) {
        puts("DROP IGNORED");
        return;
    }
    const StringView mime = dropMode == 7 ? StringView(u8"application/unknown") : offer->format(offer->formats() - 1);
    Input* input = drop.read(mime);
    Buffer content;
    if (dropMode == 5) {
        char byte;
        STD_INSIST(input->read(&byte, 1) == 1);
        puts("DROP PARTIAL");
    } else {
        input->readAll(content);
    }
    delete input;
    input = drop.read(mime);
    char extra;
    STD_INSIST(input->read(&extra, 1) == 0);
    delete input;
    printf("DROPPED %s\n", content.cStr());
    color = 0x40a060;
    window->requestFrame();
}
