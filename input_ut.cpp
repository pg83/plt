#include "fiber.h"
#include "input.h"
#include "poller.h"

#include <std/tst/ut.h>
#include <std/thr/poll_fd.h>
#include <std/mem/obj_pool.h>

#include <string>

using namespace plt;
using namespace stl;

namespace {
    struct ManualPoller final: Poller {
        void arm(PollWaiter& waiter) override {
            armedFd = waiter.fd.fd;
            callback = waiter.callback;
        }

        void cancel(PollWaiter&) override {
            armedFd = -1;
            callback = nullptr;
        }

        void timeout(u64, TimerCallback&) override {
        }

        void deadline(u64, TimerCallback&) override {
        }

        void cancel(TimerCallback&) override {
        }

        void defer(TimerCallback&) override {
        }

        void fireFd() {
            PollCallback* const ready = callback;
            callback = nullptr;
            armedFd = -1;
            ready->ready(PollFD{});
        }

        int armedFd = -1;
        PollCallback* callback = nullptr;
    };

    struct RecordingSink final: InputSink {
        explicit RecordingSink(Scheduler& scheduler_)
            : scheduler(scheduler_)
        {
        }

        void key(const KeyInput& input) override {
            order += 'K';
            keyInput = input;
            if (blockKey) {
                blockKey = false;
                scheduler.awaitReadable(blockFd, 0);
                keyResumed = true;
            }
        }

        void text(const TextInput& input) override {
            order += 'T';
            textInput = input;
        }

        void preedit(StringView text, i32 cursorBegin, i32 cursorEnd) override {
            order += 'E';
            preeditText.assign((const char*)(text.data()), text.length());
            preeditBegin = cursorBegin;
            preeditEnd = cursorEnd;
        }

        void pointerMotion(const PointerMotionInput& input) override {
            order += 'M';
            motionInput = input;
        }

        void pointerButton(const PointerButtonInput& input) override {
            order += 'B';
            buttonInput = input;
        }

        void scroll(const ScrollInput& input) override {
            order += 'S';
            scrollInput = input;
        }

        void focus(bool focused) override {
            order += 'F';
            focusValue = focused;
        }

        void pointerPresence(bool present) override {
            order += 'P';
            presenceValue = present;
        }

        void flush() override {
            order += 'X';
            ++flushes;
        }

        Scheduler& scheduler;
        std::string order;
        std::string preeditText;
        KeyInput keyInput;
        TextInput textInput;
        PointerMotionInput motionInput;
        PointerButtonInput buttonInput;
        ScrollInput scrollInput;
        i32 preeditBegin = 0;
        i32 preeditEnd = 0;
        int flushes = 0;
        int blockFd = 7;
        bool blockKey = true;
        bool keyResumed = false;
        bool focusValue = false;
        bool presenceValue = true;
    };
}

STD_TEST_SUITE(FiberInputSink) {
    STD_TEST(QueuesEveryEventInOrderAndCopiesPreedit) {
        ManualPoller poller;
        ObjPool::Ref schedulerOwner = ObjPool::fromMemory();
        Scheduler* const scheduler = Scheduler::create(*schedulerOwner, poller);
        RecordingSink target(*scheduler);
        ObjPool* const sinkOwner = ObjPool::fromMemoryRaw();
        InputSink* const sink = createFiberInputSink(*sinkOwner, *scheduler, target);

        sink->key({
            .key = InputKey::F12,
            .action = InputAction::Repeat,
            .modifiers = InputShift | InputAlt,
            .layoutCodepoint = 'x',
            .baseCodepoint = 'X',
            .shiftedCodepoint = 'X',
        });
        STD_INSIST(poller.armedFd == target.blockFd);

        sink->text({
            .codepoint = 0x1f642,
            .modifiers = InputControl,
        });
        u8 preedit[] = {'c', 'o', 'm', 'p', 'o', 's', 'e'};
        sink->preedit(StringView(preedit, sizeof(preedit)), 1, 6);
        sink->pointerMotion({
            .pixelX = 101,
            .pixelY = -23,
            .modifiers = InputSuper,
        });
        sink->pointerButton({
            .button = PointerButton::Auxiliary4,
            .pressed = true,
            .pixelX = 17,
            .pixelY = 19,
            .modifiers = InputAltGraph,
            .time = 42.5,
        });
        sink->scroll({
            .x = 1.25,
            .y = -2.5,
            .pixelX = 29,
            .pixelY = 31,
            .modifiers = InputNumLock,
            .phase = ScrollPhase::End,
            .precise = true,
            .momentum = true,
            .time = 43.5,
        });
        sink->focus(true);
        sink->pointerPresence(false);
        sink->flush();
        preedit[0] = 'X';

        // The key handler is still waiting on its descriptor, so every
        // later event must be queued rather than delivered recursively.
        STD_INSIST(target.order == "K");
        STD_INSIST(!target.keyResumed);
        poller.fireFd();

        STD_INSIST(target.keyResumed);
        STD_INSIST(target.order == "KTEMBSFPX");
        STD_INSIST(target.keyInput.key == InputKey::F12);
        STD_INSIST(target.keyInput.action == InputAction::Repeat);
        STD_INSIST(target.keyInput.modifiers == (InputShift | InputAlt));
        STD_INSIST(target.keyInput.layoutCodepoint == 'x');
        STD_INSIST(target.keyInput.shiftedCodepoint == 'X');
        STD_INSIST(target.keyInput.baseCodepoint == 'X');
        STD_INSIST(target.textInput.codepoint == 0x1f642);
        STD_INSIST(target.textInput.modifiers == InputControl);
        STD_INSIST(target.preeditText == "compose");
        STD_INSIST(target.preeditBegin == 1);
        STD_INSIST(target.preeditEnd == 6);
        STD_INSIST(target.motionInput.pixelX == 101);
        STD_INSIST(target.motionInput.pixelY == -23);
        STD_INSIST(target.motionInput.modifiers == InputSuper);
        STD_INSIST(target.buttonInput.button == PointerButton::Auxiliary4);
        STD_INSIST(target.buttonInput.pressed);
        STD_INSIST(target.buttonInput.pixelX == 17);
        STD_INSIST(target.buttonInput.pixelY == 19);
        STD_INSIST(target.buttonInput.modifiers == InputAltGraph);
        STD_INSIST(target.buttonInput.time == 42.5);
        STD_INSIST(target.scrollInput.x == 1.25);
        STD_INSIST(target.scrollInput.y == -2.5);
        STD_INSIST(target.scrollInput.pixelX == 29);
        STD_INSIST(target.scrollInput.pixelY == 31);
        STD_INSIST(target.scrollInput.modifiers == InputNumLock);
        STD_INSIST(target.scrollInput.phase == ScrollPhase::End);
        STD_INSIST(target.scrollInput.precise);
        STD_INSIST(target.scrollInput.momentum);
        STD_INSIST(target.scrollInput.time == 43.5);
        STD_INSIST(target.focusValue);
        STD_INSIST(!target.presenceValue);
        STD_INSIST(target.flushes == 1);

        delete sinkOwner;
    }

    STD_TEST(OwnerDeathCancelsPumpParkedInsideTarget) {
        ManualPoller poller;
        ObjPool::Ref schedulerOwner = ObjPool::fromMemory();
        Scheduler* const scheduler = Scheduler::create(*schedulerOwner, poller);
        RecordingSink target(*scheduler);
        ObjPool* const sinkOwner = ObjPool::fromMemoryRaw();
        InputSink* const sink = createFiberInputSink(*sinkOwner, *scheduler, target);

        sink->key({.key = InputKey::Enter});
        STD_INSIST(target.order == "K");
        STD_INSIST(!target.keyResumed);
        STD_INSIST(poller.armedFd == target.blockFd);

        delete sinkOwner;
        poller.fireFd();

        STD_INSIST(target.order == "K");
        STD_INSIST(!target.keyResumed);
    }
}
