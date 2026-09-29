#include "app.h"
#include "fiber.h"

#include <std/dbg/panic.h>
#include <std/thr/runable.h>

#include <stdio.h>
#include <string.h>

#ifdef __LLVM_INSTR_PROFILE_GENERATE
extern "C" int __llvm_profile_write_file();
#endif

using namespace plt;
using namespace plt::e2e;
using namespace stl;

namespace {
    struct Task final: public App, public Runable {
        void paint(Canvas& canvas) override;
        void key(const KeyInput& input) override;
        void run() override;
        Fiber* handle = nullptr;
        const char* mode = setting("PLT_LIFETIME", "park");
    };

    static void saveProfile() {
#ifdef __LLVM_INSTR_PROFILE_GENERATE
        __llvm_profile_write_file();
#endif
    }
}

void Task::paint(Canvas& canvas) {
    canvas.clear(0x204060);
    canvas.text(20, 40, StringView(u8"Task lifetime diagnostics"));
}

void Task::key(const KeyInput& input) {
    if (input.action != InputAction::Press || input.baseCodepoint != 'a') {
        return;
    }
    puts("INVALID LIFETIME REQUEST");
    if (strcmp(mode, "park") == 0) {
        handle->park();
    } else if (strcmp(mode, "timeout") == 0) {
        handle->parkFor(1000);
    } else {
        handle->wake();
    }
}

void Task::run() {
    if (strcmp(mode, "park") == 0 || strcmp(mode, "timeout") == 0) {
        return;
    }
    platform->scheduler()->current()->park();
    if (strcmp(mode, "release") == 0) {
        handle->release();
    } else {
        platform->scheduler()->current()->release();
    }
}

int main() {
    setPanicHandler1(saveProfile);
    Task app;
    app.open("plt-fiber-lifetime");
    app.handle = app.platform->scheduler()->create(*app.owner, app);
    app.App::run();
}
