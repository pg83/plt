/*
 * Copyright (C) 2026 pg83
 * MIT licensed
 * See the file LICENSE for the full license.
 */

#include "platform.h"

namespace plt {
    Platform* createNativePlatform(stl::ObjPool& owner, PlatformEvents& events);

    Platform* Platform::create(stl::ObjPool& owner, PlatformEvents& events) {
        return createNativePlatform(owner, events);
    }
}
