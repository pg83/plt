/*
 * Copyright (C) 2026 pg83
 * MIT licensed
 * See the file LICENSE for the full license.
 */

#pragma once

#include <std/sys/types.h>

namespace plt {
    enum class InputKey : u8 {
        Unknown,
        Printable,
        Escape,
        Enter,
        Backspace,
        Tab,
        Insert,
        Delete,
        Home,
        End,
        Up,
        Down,
        Left,
        Right,
        PageUp,
        PageDown,
        F1,
        F2,
        F3,
        F4,
        F5,
        F6,
        F7,
        F8,
        F9,
        F10,
        F11,
        F12,
        F13,
        F14,
        F15,
        F16,
        F17,
        F18,
        F19,
        F20,
        Keypad0,
        Keypad1,
        Keypad2,
        Keypad3,
        Keypad4,
        Keypad5,
        Keypad6,
        Keypad7,
        Keypad8,
        Keypad9,
        KeypadDecimal,
        KeypadDivide,
        KeypadMultiply,
        KeypadSubtract,
        KeypadAdd,
        KeypadEnter,
        KeypadEqual,
        CapsLock,
        ScrollLock,
        NumLock,
        PrintScreen,
        Pause,
        Menu,
        LeftShift,
        LeftControl,
        LeftAlt,
        LeftSuper,
        RightShift,
        RightControl,
        RightAlt,
        RightSuper,
        Count
    };

    enum class InputAction : u8 {
        Press,
        Repeat,
        Release
    };

    enum InputModifier : u16 {
        InputShift = 1 << 0,
        InputControl = 1 << 1,
        InputAlt = 1 << 2,
        InputSuper = 1 << 3,
        InputCapsLock = 1 << 4,
        InputNumLock = 1 << 5,
        InputAltGraph = 1 << 6
    };

    struct KeyInput {
        InputKey key = InputKey::Unknown;
        InputAction action = InputAction::Press;
        u16 modifiers = 0;
        u32 layoutCodepoint = 0;
        u32 baseCodepoint = 0;
    };

    struct TextInput {
        u32 codepoint = 0;
        u16 modifiers = 0;
    };

    enum class PointerButton : u8 {
        Primary,
        Secondary,
        Middle,
        Auxiliary1,
        Auxiliary2,
        Auxiliary3,
        Auxiliary4,
        Auxiliary5
    };

    struct PointerMotionInput {
        int pixelX = 0;
        int pixelY = 0;
        u16 modifiers = 0;
    };

    struct PointerButtonInput {
        PointerButton button = PointerButton::Primary;
        bool pressed = false;
        int pixelX = 0;
        int pixelY = 0;
        u16 modifiers = 0;
        double time = 0;
    };

    struct ScrollInput {
        double x = 0;
        double y = 0;
        int pixelX = 0;
        int pixelY = 0;
        u16 modifiers = 0;
    };

    struct InputSink {
        virtual void key(const KeyInput& input) = 0;
        virtual void text(const TextInput& input) = 0;
        virtual void pointerMotion(const PointerMotionInput& input) = 0;
        virtual void pointerButton(const PointerButtonInput& input) = 0;
        virtual void scroll(const ScrollInput& input) = 0;
        virtual void focus(bool focused) = 0;
        virtual void pointerPresence(bool present) = 0;
        virtual void flush() = 0;
    };
}
