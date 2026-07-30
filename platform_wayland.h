#pragma once

namespace stl {
    class ObjPool;
}

namespace plt {
    struct Platform;

    Platform* createBackendPlatform(stl::ObjPool& owner);
}
