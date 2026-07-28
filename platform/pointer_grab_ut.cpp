/*
 * Copyright (C) 2026 pg83
 * MIT licensed
 * See the file LICENSE for the full license.
 */

#include "pointer_grab.h"

#include <std/tst/ut.h>

using namespace stl;

namespace plt {
    STD_TEST_SUITE(PointerGrab) {
        STD_TEST(RoutesUncapturedEventsToFocus) {
            PointerGrab state;
            int window;

            state.enter(&window);

            STD_INSIST(state.focusTarget() == &window);
            STD_INSIST(state.eventTarget() == &window);
            state.leave(&window);
            STD_INSIST(state.eventTarget() == nullptr);
        }

        STD_TEST(HoldsTargetFromFirstPressThroughRelease) {
            PointerGrab state;
            int window;

            state.enter(&window);
            STD_INSIST(state.buttonTarget(true) == &window);
            state.leave(&window);

            STD_INSIST(state.focusTarget() == nullptr);
            STD_INSIST(state.eventTarget() == &window);
            STD_INSIST(state.buttonTarget(false) == &window);
            STD_INSIST(state.eventTarget() == nullptr);
        }

        STD_TEST(HoldsTargetUntilEveryButtonIsReleased) {
            PointerGrab state;
            int window;

            state.enter(&window);
            STD_INSIST(state.buttonTarget(true) == &window);
            STD_INSIST(state.buttonTarget(true) == &window);
            state.leave(&window);
            STD_INSIST(state.buttonTarget(false) == &window);
            STD_INSIST(state.eventTarget() == &window);
            STD_INSIST(state.buttonTarget(false) == &window);
            STD_INSIST(state.eventTarget() == nullptr);
        }

        STD_TEST(NewFocusDoesNotStealActiveGrab) {
            PointerGrab state;
            int first;
            int second;

            state.enter(&first);
            STD_INSIST(state.buttonTarget(true) == &first);
            state.leave(&first);
            state.enter(&second);

            STD_INSIST(state.focusTarget() == &second);
            STD_INSIST(state.eventTarget() == &first);
            STD_INSIST(state.buttonTarget(false) == &first);
            STD_INSIST(state.eventTarget() == &second);
        }

        STD_TEST(RemovingGrabbedTargetCancelsCapture) {
            PointerGrab state;
            int window;

            state.enter(&window);
            STD_INSIST(state.buttonTarget(true) == &window);
            state.remove(&window);

            STD_INSIST(state.focusTarget() == nullptr);
            STD_INSIST(state.eventTarget() == nullptr);
            STD_INSIST(state.pressedButtons == 0);
        }
    }
}
