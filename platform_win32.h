/*
 * Copyright (C) 2026 pg83
 * MIT licensed
 * See the file LICENSE for the full license.
 */

#pragma once

#include "platform.h"

namespace plt {
    Platform* createWin32Platform(stl::ObjPool& owner, PlatformEvents& events);
}
