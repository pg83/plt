#pragma once

namespace stl {
    class ObjPool;
}

namespace plt {
    struct Platform;

    Platform* createCocoaPlatform(stl::ObjPool& owner);
}
