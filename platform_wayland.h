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
    struct Platform;

    Platform* createWaylandPlatform(stl::ObjPool& owner);
}
