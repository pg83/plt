#include "platform_headless.h"

#include "poller.h"

#include <std/thr/poll_fd.h>
#include <std/mem/obj_pool.h>

#include <vector>
#include <algorithm>

using namespace plt;
using namespace stl;

namespace {
    struct PollerHeadless final: Poller {
        void arm(PollFD, PollCallback&) override {
        }

        void disarm(int) override {
        }

        void timeout(u64, TimerCallback&) override {
        }

        void deadline(u64, TimerCallback&) override {
        }

        void cancel(TimerCallback&) override {
        }
    };

    struct WindowHeadlessImpl final: WindowHeadless {
        explicit WindowHeadlessImpl(const WindowOptions& options);

        void requestShow() override;
        void requestClose() override;
        void requestFrame() override;
        void requestTitle(StringView title) override;
        void requestAttention() override;
        void requestRestore() override;
        void requestIconify() override;
        void requestMove(i32 x, i32 y) override;
        void requestFocus() override;
        void requestMaximized(bool maximized) override;
        void requestFullscreen(bool fullscreen) override;
        void requestResize(u32 width, u32 height) override;
        void requestMinimumSize(u32 width, u32 height) override;
        void requestResizeUnit(u32 width, u32 height, u32 baseWidth, u32 baseHeight) override;
        void requestReadPrimary(ClipboardRead& read) override;
        void requestReadClipboard(ClipboardRead& read) override;
        void cancelClipboardRead(ClipboardRead& read) override;
        void requestWritePrimary(StringView content) override;
        void requestWriteClipboard(StringView content) override;
        void requestWriteClipboard(StringView mime, StringView content) override;
        void requestPointerIcon(PointerIcon icon) override;
        void requestTextInputRect(i32 x, i32 y, u32 width, u32 height) override;
        WindowInfo info() const override;
        RenderContext renderContext() const override;

        bool dispatchFrame() override;
        bool framePending() const override;
        void configure(const WindowInfo& info) override;
        void failNextPresentation() override;
        HeadlessFrame presentedFrame() const override;

        void resizeBackBuffer();
        void restoreSize();

        WindowEvents* events = nullptr;
        FrameCallback* frame = nullptr;
        WindowInfo info_;
        WindowInfo restored_;
        mutable HeadlessRenderTarget target_;
        std::vector<u8> front_;
        std::vector<u8> back_;
        u32 frontWidth_ = 0;
        u32 frontHeight_ = 0;
        u64 generation_ = 0;
        bool pending_ = false;
        bool failNext_ = false;
        bool haveRestored_ = false;
        bool closed_ = false;
    };

    struct PlatformHeadless final: Platform {
        void run() override {
            running = true;
            while (running) {
                bool dispatched = false;
                for (WindowHeadlessImpl* window : windows) {
                    if (window->framePending()) {
                        window->dispatchFrame();
                        dispatched = true;
                    }
                }
                if (!dispatched) {
                    break;
                }
            }
        }

        void stop() override {
            running = false;
        }

        Poller* poller() override {
            return &poller_;
        }

        Window* createWindow(ObjPool& windowOwner, const WindowOptions& options) override {
            WindowHeadlessImpl* const window = windowOwner.make<WindowHeadlessImpl>(options);
            windows.push_back(window);
            return window;
        }

        PollerHeadless poller_;
        std::vector<WindowHeadlessImpl*> windows;
        bool running = false;
    };
}

WindowHeadlessImpl::WindowHeadlessImpl(const WindowOptions& options)
    : events(options.events)
    , frame(options.frame)
{
    info_.x = 10;
    info_.y = 20;
    info_.width = std::max(1u, options.width);
    info_.height = std::max(1u, options.height);
    info_.screenPixelWidth = 1920;
    info_.screenPixelHeight = 1080;
    info_.contentScale = 1.0f;
}

void WindowHeadlessImpl::requestShow() {
    requestFrame();
}

void WindowHeadlessImpl::requestClose() {
    closed_ = true;
    if (events != nullptr) {
        events->close();
    }
}

void WindowHeadlessImpl::requestFrame() {
    if (!closed_) {
        pending_ = true;
    }
}

void WindowHeadlessImpl::requestTitle(StringView) {
}

void WindowHeadlessImpl::requestAttention() {
}

void WindowHeadlessImpl::requestRestore() {
    info_.iconified = false;
}

void WindowHeadlessImpl::requestIconify() {
    info_.iconified = true;
}

void WindowHeadlessImpl::requestMove(i32 x, i32 y) {
    info_.x = x;
    info_.y = y;
}

void WindowHeadlessImpl::requestFocus() {
    info_.focused = true;
}

void WindowHeadlessImpl::restoreSize() {
    if (!haveRestored_) {
        return;
    }
    info_.width = restored_.width;
    info_.height = restored_.height;
    haveRestored_ = false;
    requestFrame();
}

void WindowHeadlessImpl::requestMaximized(bool maximized) {
    if (maximized == info_.maximized) {
        return;
    }
    if (maximized) {
        if (!haveRestored_) {
            restored_ = info_;
            haveRestored_ = true;
        }
        info_.width = std::max(1u, info_.screenPixelWidth);
        info_.height = std::max(1u, info_.screenPixelHeight);
    } else if (!info_.fullscreen) {
        restoreSize();
    }
    info_.maximized = maximized;
    requestFrame();
}

void WindowHeadlessImpl::requestFullscreen(bool fullscreen) {
    if (fullscreen == info_.fullscreen) {
        return;
    }
    if (fullscreen) {
        if (!haveRestored_) {
            restored_ = info_;
            haveRestored_ = true;
        }
        info_.width = std::max(1u, info_.screenPixelWidth);
        info_.height = std::max(1u, info_.screenPixelHeight);
    } else if (!info_.maximized) {
        restoreSize();
    }
    info_.fullscreen = fullscreen;
    requestFrame();
}

void WindowHeadlessImpl::requestResize(u32 width, u32 height) {
    if (width == 0 || height == 0) {
        return;
    }
    info_.width = width;
    info_.height = height;
    requestFrame();
}

void WindowHeadlessImpl::requestMinimumSize(u32, u32) {
}

void WindowHeadlessImpl::requestResizeUnit(u32, u32, u32, u32) {
}

void WindowHeadlessImpl::requestReadPrimary(ClipboardRead& read) {
    read.done(false);
}

void WindowHeadlessImpl::requestReadClipboard(ClipboardRead& read) {
    read.done(false);
}

void WindowHeadlessImpl::cancelClipboardRead(ClipboardRead&) {
}

void WindowHeadlessImpl::requestWritePrimary(StringView) {
}

void WindowHeadlessImpl::requestWriteClipboard(StringView) {
}

void WindowHeadlessImpl::requestWriteClipboard(StringView, StringView) {
}

void WindowHeadlessImpl::requestPointerIcon(PointerIcon) {
}

void WindowHeadlessImpl::requestTextInputRect(i32, i32, u32, u32) {
}

WindowInfo WindowHeadlessImpl::info() const {
    return info_;
}

RenderContext WindowHeadlessImpl::renderContext() const {
    return {
        .backend = RenderBackend::Headless,
        .connection = nullptr,
        .window = &target_,
    };
}

void WindowHeadlessImpl::resizeBackBuffer() {
    const size_t length = (size_t)(info_.width) * info_.height * 3;
    back_.resize(length);
    target_.pixels = back_.data();
    target_.length = back_.size();
    target_.width = info_.width;
    target_.height = info_.height;
    target_.stride = info_.width * 3;
}

bool WindowHeadlessImpl::dispatchFrame() {
    if (!pending_) {
        return false;
    }
    pending_ = false;
    resizeBackBuffer();
    const bool fail = failNext_;
    failNext_ = false;
    u8* const pixels = target_.pixels;
    const size_t length = target_.length;
    if (fail) {
        target_.pixels = nullptr;
        target_.length = 0;
    }
    const WindowInfo frameInfo = info_;
    const bool presented = frame != nullptr && frame->frame(frameInfo);
    target_.pixels = pixels;
    target_.length = length;
    if (!presented) {
        return false;
    }
    front_.swap(back_);
    frontWidth_ = frameInfo.width;
    frontHeight_ = frameInfo.height;
    ++generation_;
    target_.pixels = back_.data();
    target_.length = back_.size();
    return true;
}

bool WindowHeadlessImpl::framePending() const {
    return pending_;
}

void WindowHeadlessImpl::configure(const WindowInfo& info) {
    info_ = info;
    info_.width = std::max(1u, info_.width);
    info_.height = std::max(1u, info_.height);
    if (!(info_.contentScale > 0.0f)) {
        info_.contentScale = 1.0f;
    }
    requestFrame();
}

void WindowHeadlessImpl::failNextPresentation() {
    failNext_ = true;
    requestFrame();
}

HeadlessFrame WindowHeadlessImpl::presentedFrame() const {
    return {
        .pixels = front_.data(),
        .length = front_.size(),
        .width = frontWidth_,
        .height = frontHeight_,
        .stride = frontWidth_ * 3,
        .generation = generation_,
    };
}

Platform* plt::createHeadlessPlatform(ObjPool& owner) {
    return owner.make<PlatformHeadless>();
}
