/*
 * Copyright (C) 2026 pg83
 * MIT licensed
 * See the file LICENSE for the full license.
 */

#include "platform.h"

#if defined(__APPLE__)
    #include "platform_cocoa.h"
#elif defined(_WIN32)
    #include "platform_win32.h"
#elif defined(__linux__)
    #include "platform_wayland.h"
#else
    #error Unsupported platform
#endif

using namespace plt;

Platform* Platform::create(stl::ObjPool& owner, PlatformEvents& events) {
#if defined(__APPLE__)
    return createCocoaPlatform(owner, events);
#elif defined(_WIN32)
    return createWin32Platform(owner, events);
#else
    return createWaylandPlatform(owner, events);
#endif
}
