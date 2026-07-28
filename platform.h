/*
 * Copyright (C) 2026 pg83
 * MIT licensed
 * See the file LICENSE for the full license.
 */

#pragma once

#include "window.h"

#include <std/sys/types.h>

namespace stl {
    class ObjPool;
}

namespace plt {
    enum PollMode : u8 {
        PollRead = 1 << 0,
        PollWrite = 1 << 1,
        PollError = 1 << 2,
        PollHangup = 1 << 3
    };

    struct FDReady {
        int fd = -1;
        int what = 0;
    };

    struct PlatformEvents {
        virtual void fdReady(const FDReady& event) = 0;
        virtual void timeout() = 0;
        virtual void check() = 0;
    };

    struct Platform {
        virtual Window* createWindow(stl::ObjPool& owner, const WindowOptions& options) = 0;

        virtual void arm(int fd, int mode) = 0;
        virtual void disarm(int fd) = 0;
        virtual void timeout(u64 microseconds) = 0;
        virtual void deadline(u64 monotonicMicroseconds) = 0;
        virtual void run() = 0;
        virtual void stop() = 0;

        static Platform* create(stl::ObjPool& owner, PlatformEvents& events);
    };
}
