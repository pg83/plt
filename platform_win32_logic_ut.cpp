#include "platform_win32_logic.h"

using namespace plt::win32_detail;

namespace {
    constexpr wchar_t terminated[] = {L'a', L'b', 0, L'c'};
    constexpr wchar_t unterminated[] = {L'a', L'b', L'c'};

    static_assert(boundedWideLength(terminated, 4) == 2);
    static_assert(boundedWideLength(unterminated, 3) == 3);

    static_assert(!extendedKey(0));
    static_assert(extendedKey(1ll << 24));

    constexpr bool captureSequence() {
        unsigned buttons = 0;
        return updateButtonMask(buttons, 1u, true) == CaptureChange::Acquire
            && updateButtonMask(buttons, 1u, true) == CaptureChange::None
            && updateButtonMask(buttons, 2u, true) == CaptureChange::None
            && updateButtonMask(buttons, 1u, false) == CaptureChange::None
            && updateButtonMask(buttons, 2u, false) == CaptureChange::Release;
    }

    static_assert(captureSequence());
}
