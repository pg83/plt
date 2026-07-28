#pragma once

#include <stddef.h>

namespace plt::win32_detail {
    enum class CaptureChange {
        None,
        Acquire,
        Release,
    };

    constexpr size_t boundedWideLength(const wchar_t* value, size_t capacity) {
        size_t length = 0;
        while (length != capacity && value[length] != 0) {
            ++length;
        }
        return length;
    }

    constexpr bool extendedKey(long long messageData) {
        return (messageData & (1ll << 24)) != 0;
    }

    constexpr CaptureChange updateButtonMask(unsigned& buttons, unsigned button, bool pressed) {
        const unsigned previous = buttons;
        if (pressed) {
            buttons |= button;
        } else {
            buttons &= ~button;
        }
        if (previous == 0 && buttons != 0) {
            return CaptureChange::Acquire;
        }
        if (previous != 0 && buttons == 0) {
            return CaptureChange::Release;
        }
        return CaptureChange::None;
    }
}
