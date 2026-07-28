#pragma once

namespace stl {
    class ObjPool;
}

namespace plt {
    struct Platform;

    Platform* createWin32Platform(stl::ObjPool& owner);
}
