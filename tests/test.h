#pragma once

#include "input.h"
#include "platform.h"
#include "poller.h"
#include "window.h"

#include <std/lib/buffer.h>
#include <std/mem/obj_pool.h>
#include <std/str/view.h>

namespace plt::test {
    enum class Command : u32 {
        DeferInitialConfigure,
        ReleaseInitialConfigure,
        QueryInitialConfigure,
        PointerEnter,
        PreferredScale,
        QuerySelection,
        QueryMinimum,
        OfferSelection,
        OfferPlainSelection,
        OfferUnsupportedSelection,
        ReleaseRead,
        RequestSourceData,
        RequestBrokenSourceData,
        CancelSources,
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
        InvalidKeymap,
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

    Reply command(int fd, Command value);
    void pump(Platform& platform);
    stl::Buffer repeated(size_t size, u8 value);

    struct Client {
        explicit Client(
            int controlFd,
            u32 width = 800,
            u32 minimum = 1,
            WindowEvents* events = nullptr,
            InputSink* input = nullptr,
            bool waitForConfigure = true
        );

        int controlFd;
        stl::ObjPool::Ref owner;
        Platform* platform = nullptr;
        Window* window = nullptr;
    };

    struct ReadSink final: ClipboardRead {
        bool data(stl::StringView chunk) override {
            content.append(chunk.data(), chunk.length());
            return true;
        }

        void done(bool success_) override {
            success = success_;
            complete = true;
        }

        stl::Buffer content;
        bool complete = false;
        bool success = false;
    };

    struct RejectSink final: ClipboardRead {
        bool data(stl::StringView) override {
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

    struct EventSink final: WindowEvents {
        void close() override {
            ++closeCount;
        }

        void resized(const WindowInfo& info) override {
            ++resizeCount;
            lastInfo = info;
        }

        void redraw() override {
            ++redrawCount;
        }

        void frame() override {
            ++frameCount;
        }

        WindowInfo lastInfo;
        u32 closeCount = 0;
        u32 resizeCount = 0;
        u32 redrawCount = 0;
        u32 frameCount = 0;
    };

    struct InputRecorder final: InputSink {
        void key(const KeyInput& input) override {
            lastKey = input;
            if (input.action == InputAction::Press) {
                pressedKey = input;
                ++pressCount;
            } else if (input.action == InputAction::Repeat) {
                ++repeatCount;
            } else {
                ++releaseCount;
            }
        }

        void text(const TextInput& input) override {
            lastText = input;
            ++textCount;
        }

        void pointerMotion(const PointerMotionInput& input) override {
            lastMotion = input;
            ++motionCount;
        }

        void pointerButton(const PointerButtonInput& input) override {
            lastButton = input;
            if (input.pressed) {
                ++buttonPressCount;
            } else {
                ++buttonReleaseCount;
            }
        }

        void scroll(const ScrollInput& input) override {
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

        KeyInput pressedKey;
        KeyInput lastKey;
        TextInput lastText;
        PointerMotionInput lastMotion;
        PointerButtonInput lastButton;
        ScrollInput lastScroll;
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

    struct StopOnClose final: WindowEvents {
        explicit StopOnClose(Platform*& platform_): platform(platform_) {}

        void close() override {
            closed = true;
            platform->stop();
        }

        void resized(const WindowInfo&) override {}
        void redraw() override {}
        void frame() override {}

        Platform*& platform;
        bool closed = false;
    };

    bool nonblockingShow(int fd);
    bool windowApi(int fd);
    bool frameApi(int fd);
    bool pointerInput(int fd);
    bool keyboardInput(int fd);
    bool localSelections(int fd);
    bool missingSelections(int fd);
    bool rejectedSelection(int fd);
    bool pollerApi(int fd);
    bool deferredClipboard(int fd);
    bool fractionalRounding(int fd);
    bool minimumAfterScale(int fd);
    bool asynchronousRead(int fd);
    bool asynchronousPrimary(int fd);
    bool cancelAsynchronousRead(int fd);
    bool cancelReadyClipboardRead(int fd);
    bool asynchronousWrite(int fd);
    bool brokenClipboardConsumer(int fd);
    bool flushBackpressure(int fd);
    bool queuedWaylandEvent(int fd);
    bool plainMimeSelection(int fd);
    bool unsupportedMimeSelection(int fd);
    bool sourceCancellation(int fd);
    bool invalidKeymap(int fd);
    bool multipleWindows(int fd);
}
