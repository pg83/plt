#pragma once

namespace stl {
    class ObjPool;
}

namespace plt {
#if defined(__APPLE__)
    constexpr bool cocoa = true;
    constexpr bool wayland = false;
#elif defined(__linux__)
    constexpr bool cocoa = false;
    constexpr bool wayland = true;
#else
    #error Unsupported platform
#endif

    struct Poller;
    struct Window;
    struct WindowOptions;

    struct Platform {
        virtual void run() = 0;
        virtual void stop() = 0;

        virtual Poller* poller() = 0;
        virtual Window* createWindow(stl::ObjPool& owner, const WindowOptions& options) = 0;

        static Platform* create(stl::ObjPool& owner);
    };
}
