#pragma once

#include <std/str/view.h>
#include <std/sys/types.h>

namespace plt {
    struct InputSink;

    enum class RenderBackend : u8 {
        Wayland,
        Cocoa
    };

    struct RenderContext {
        RenderBackend backend;
        void* connection;
        void* window;
    };

    enum class PointerIcon : u8 {
        Text,
        Link
    };

    struct WindowInfo {
        i32 x = 0;
        i32 y = 0;
        u32 width = 0;
        u32 height = 0;
        u32 screenPixelWidth = 0;
        u32 screenPixelHeight = 0;
        float contentScale = 1.0f;
        bool focused = false;
        bool iconified = false;
        bool maximized = false;
        bool fullscreen = false;
        bool tiled = false;
    };

    struct WindowEvents {
        virtual void close() = 0;
        virtual void resized(const WindowInfo& info) = 0;
        virtual void redraw() = 0;
        virtual void frame() = 0;
    };

    struct ClipboardRead {
        // chunk is valid only for the duration of this call. Returning false
        // stops the transfer and completes it with success=false.
        virtual bool data(stl::StringView chunk) = 0;
        // Called exactly once unless the transfer is cancelled.
        virtual void done(bool success) = 0;
    };

    struct WindowOptions {
        stl::StringView appId;
        stl::StringView title;
        u32 width = 800;
        u32 height = 600;
        u32 minimumWidth = 1;
        u32 minimumHeight = 1;
        InputSink* input = nullptr;
        WindowEvents* events = nullptr;
    };

    struct Window {
        virtual void show() = 0;
        virtual void requestClose() = 0;
        virtual bool requestFrame() = 0;
        virtual void cancelFrame() = 0;

        virtual void setTitle(stl::StringView title) = 0;
        virtual void requestAttention() = 0;
        virtual void requestRedraw() = 0;
        virtual void restore() = 0;
        virtual void iconify() = 0;
        virtual void move(i32 x, i32 y) = 0;
        virtual void focus() = 0;
        virtual void setMaximized(bool maximized) = 0;
        virtual void setFullscreen(bool fullscreen) = 0;
        virtual void resize(u32 width, u32 height) = 0;
        virtual void setMinimumSize(u32 width, u32 height) = 0;
        virtual void setResizeUnit(u32 width, u32 height, u32 baseWidth, u32 baseHeight) = 0;
        virtual WindowInfo info() const = 0;

        virtual void readPrimary(ClipboardRead& read) = 0;
        virtual void readClipboard(ClipboardRead& read) = 0;
        // After this returns, read receives no more callbacks for cancelled transfers.
        virtual void cancelClipboardRead(ClipboardRead& read) = 0;
        virtual void writePrimary(stl::StringView content) = 0;
        virtual void writeClipboard(stl::StringView content) = 0;
        virtual void pointerIcon(PointerIcon icon) = 0;

        virtual RenderContext renderContext() const = 0;
    };
}
