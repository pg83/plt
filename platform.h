/*
 * Copyright (C) 2026 pg83
 * MIT licensed
 * See the file LICENSE for the full license.
 */

#pragma once

namespace stl {
    class ObjPool;
}

namespace plt {
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
