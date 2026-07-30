#include "platform.h"

using namespace plt;

namespace plt {
    Platform* createBackendPlatform(stl::ObjPool& owner);
}

Platform* Platform::create(stl::ObjPool& owner) {
    return createBackendPlatform(owner);
}
