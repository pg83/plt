/*
 * Copyright (C) 2026 pg83
 * MIT licensed
 * See the file LICENSE for the full license.
 */

#include "platform.h"

#include <std/alg/minmax.h>
#include <std/lib/buffer.h>
#include <std/lib/vector.h>
#include <std/mem/obj_pool.h>
#include <std/sym/i_map.h>
#include <std/sys/crt.h>

#import <AppKit/AppKit.h>
#import <CoreVideo/CoreVideo.h>
#import <QuartzCore/CAMetalLayer.h>

#include <errno.h>
#include <limits.h>
#include <poll.h>

using namespace stl;

namespace plt {
    void cocoaCloseImpl(void* owner);
    void cocoaResizeImpl(void* owner);
    void cocoaFocusImpl(void* owner, bool focused);
    void cocoaKeyImpl(void* owner, NSEvent* event, bool pressed);
    void cocoaFlagsImpl(void* owner, NSEvent* event);
    void cocoaPointerImpl(void* owner, NSEvent* event);
    void cocoaButtonImpl(void* owner, NSEvent* event, bool pressed);
    void cocoaScrollImpl(void* owner, NSEvent* event);
    void cocoaPointerPresenceImpl(void* owner, bool present);
}

@interface PltWindowDelegate: NSObject <NSWindowDelegate>
@property(nonatomic, assign) void* owner;
@end

@interface PltView: NSView
@property(nonatomic, assign) void* owner;
@property(nonatomic, strong) NSTrackingArea* tracking;
@end

@implementation PltWindowDelegate
- (BOOL)windowShouldClose:(NSWindow*)sender {
    (void)sender;
    plt::cocoaCloseImpl(self.owner);
    return NO;
}
- (void)windowDidResize:(NSNotification*)notification {
    (void)notification;
    plt::cocoaResizeImpl(self.owner);
}
- (void)windowDidChangeBackingProperties:(NSNotification*)notification {
    (void)notification;
    plt::cocoaResizeImpl(self.owner);
}
- (void)windowDidBecomeKey:(NSNotification*)notification {
    (void)notification;
    plt::cocoaFocusImpl(self.owner, true);
}
- (void)windowDidResignKey:(NSNotification*)notification {
    (void)notification;
    plt::cocoaFocusImpl(self.owner, false);
}
@end

@implementation PltView
+ (Class)layerClass {
    return [CAMetalLayer class];
}
- (BOOL)wantsUpdateLayer {
    return YES;
}
- (BOOL)acceptsFirstResponder {
    return YES;
}
- (void)updateTrackingAreas {
    if (self.tracking != nil) {
        [self removeTrackingArea:self.tracking];
    }
    self.tracking = [[NSTrackingArea alloc]
        initWithRect:self.bounds
            options:NSTrackingMouseEnteredAndExited | NSTrackingMouseMoved | NSTrackingActiveInKeyWindow
              owner:self
           userInfo:nil];
    [self addTrackingArea:self.tracking];
    [super updateTrackingAreas];
}
- (void)keyDown:(NSEvent*)event {
    plt::cocoaKeyImpl(self.owner, event, true);
}
- (void)keyUp:(NSEvent*)event {
    plt::cocoaKeyImpl(self.owner, event, false);
}
- (void)flagsChanged:(NSEvent*)event {
    plt::cocoaFlagsImpl(self.owner, event);
}
- (void)mouseMoved:(NSEvent*)event {
    plt::cocoaPointerImpl(self.owner, event);
}
- (void)mouseDragged:(NSEvent*)event {
    plt::cocoaPointerImpl(self.owner, event);
}
- (void)rightMouseDragged:(NSEvent*)event {
    plt::cocoaPointerImpl(self.owner, event);
}
- (void)otherMouseDragged:(NSEvent*)event {
    plt::cocoaPointerImpl(self.owner, event);
}
- (void)mouseDown:(NSEvent*)event {
    plt::cocoaButtonImpl(self.owner, event, true);
}
- (void)mouseUp:(NSEvent*)event {
    plt::cocoaButtonImpl(self.owner, event, false);
}
- (void)rightMouseDown:(NSEvent*)event {
    plt::cocoaButtonImpl(self.owner, event, true);
}
- (void)rightMouseUp:(NSEvent*)event {
    plt::cocoaButtonImpl(self.owner, event, false);
}
- (void)otherMouseDown:(NSEvent*)event {
    plt::cocoaButtonImpl(self.owner, event, true);
}
- (void)otherMouseUp:(NSEvent*)event {
    plt::cocoaButtonImpl(self.owner, event, false);
}
- (void)scrollWheel:(NSEvent*)event {
    plt::cocoaScrollImpl(self.owner, event);
}
- (void)mouseEntered:(NSEvent*)event {
    (void)event;
    plt::cocoaPointerPresenceImpl(self.owner, true);
}
- (void)mouseExited:(NSEvent*)event {
    (void)event;
    plt::cocoaPointerPresenceImpl(self.owner, false);
}
@end

namespace plt {
    namespace {
        struct PlatformImpl;
        struct WindowImpl;

        struct ArmedFD {
            int fd = -1;
            int mode = 0;
        };

        struct WindowImpl final: public Window {
            WindowImpl(PlatformImpl& platform, const WindowOptions& options);
            ~WindowImpl();

            void show() override;
            void requestClose() override;
            bool requestFrame() override;
            void cancelFrame() override;
            void setTitle(StringView title) override;
            void requestAttention() override;
            void requestRedraw() override;
            void restore() override;
            void iconify() override;
            void move(i32 x, i32 y) override;
            void focus() override;
            void setMaximized(bool maximized) override;
            void setFullscreen(bool fullscreen) override;
            void resize(u32 width, u32 height) override;
            void setMinimumSize(u32 width, u32 height) override;
            void setResizeUnit(u32 width, u32 height, u32 baseWidth, u32 baseHeight) override;
            WindowInfo info() const override;
            StringView readPrimary() override;
            StringView readClipboard() override;
            void writePrimary(StringView content) override;
            void writeClipboard(StringView content) override;
            void pointerIcon(PointerIcon icon) override;
            RenderContext renderContext() const override;

            void close();
            void resized();
            void focused(bool value);
            void key(NSEvent* event, bool pressed);
            void flags(NSEvent* event);
            void pointer(NSEvent* event);
            void button(NSEvent* event, bool pressed);
            void scroll(NSEvent* event);
            void pointerPresence(bool present);
            void deliverFrame();
            u16 modifiers(NSEventModifierFlags flags) const;
            InputKey inputKey(NSEvent* event) const;
            u32 firstCodepoint(NSString* string) const;
            NSPoint pointerPosition(NSEvent* event) const;
            StringView readPasteboard(NSPasteboard* pasteboard);
            void writePasteboard(NSPasteboard* pasteboard, StringView content);

            PlatformImpl& platform;
            InputSink* input = nullptr;
            WindowEvents* events = nullptr;
            NSWindow* window = nil;
            PltView* view = nil;
            PltWindowDelegate* delegate = nil;
            CVDisplayLinkRef displayLink = nullptr;
            Buffer text;
            u64 modifierState = 0;
            bool framePending = false;
            bool maximized = false;
        };

        struct PlatformImpl final: public Platform {
            PlatformImpl(ObjPool& owner, PlatformEvents& events);

            Window* createWindow(ObjPool& owner, const WindowOptions& options) override;
            void arm(int fd, int mode) override;
            void disarm(int fd) override;
            void timeout(u64 microseconds) override;
            void deadline(u64 monotonicMicroseconds) override;
            void run() override;
            void stop() override;

            void dispatchFDs();
            void dispatchTimeout();
            double waitSeconds() const;

            PlatformEvents& events;
            IntMap<ArmedFD> armed;
            Vector<struct pollfd> pollFDs;
            u64 minDeadline = 0;
            bool running = false;
        };

        NSString* stringFromView(StringView value) {
            return [[NSString alloc] initWithBytes:value.data()
                                           length:value.length()
                                         encoding:NSUTF8StringEncoding];
        }

        CVReturn displayLinkCallback(CVDisplayLinkRef, const CVTimeStamp*, const CVTimeStamp*, CVOptionFlags, CVOptionFlags*, void* context) {
            WindowImpl* const window = (WindowImpl*)(context);
            CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, ^{
                window->deliverFrame();
            });
            CFRunLoopWakeUp(CFRunLoopGetMain());
            return kCVReturnSuccess;
        }
    }

    PlatformImpl::PlatformImpl(ObjPool& owner, PlatformEvents& events_)
        : events(events_)
        , armed(ObjPool::create(&owner))
    {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        [NSApp finishLaunching];
    }

    Window* PlatformImpl::createWindow(ObjPool& owner, const WindowOptions& options) {
        return owner.make<WindowImpl>(*this, options);
    }

    void PlatformImpl::arm(int fd, int mode) {
        armed[fd] = {fd, mode};
    }

    void PlatformImpl::disarm(int fd) {
        armed.erase(fd);
    }

    void PlatformImpl::timeout(u64 microseconds) {
        deadline(monotonicNowUs() + microseconds);
    }

    void PlatformImpl::deadline(u64 value) {
        if (value == 0) {
            value = monotonicNowUs();
        }
        if (minDeadline == 0 || value < minDeadline) {
            minDeadline = value;
        }
    }

    double PlatformImpl::waitSeconds() const {
        if (minDeadline == 0) {
            return 0.01;
        }
        const u64 now = monotonicNowUs();
        if (minDeadline <= now) {
            return 0;
        }
        return min(0.01, (minDeadline - now) / 1'000'000.0);
    }

    void PlatformImpl::dispatchFDs() {
        pollFDs.clear();
        armed.visit([this](const ArmedFD& source) {
            short native = 0;
            if (source.mode & PollRead) {
                native |= POLLIN;
            }
            if (source.mode & PollWrite) {
                native |= POLLOUT;
            }
            pollFDs.pushBack({source.fd, native, 0});
        });
        if (pollFDs.empty() || poll(pollFDs.mutData(), pollFDs.length(), 0) <= 0) {
            return;
        }
        for (const struct pollfd& source : pollFDs) {
            int what = 0;
            if (source.revents & POLLIN) {
                what |= PollRead;
            }
            if (source.revents & POLLOUT) {
                what |= PollWrite;
            }
            if (source.revents & (POLLERR | POLLNVAL)) {
                what |= PollError;
            }
            if (source.revents & POLLHUP) {
                what |= PollHangup;
            }
            if (what != 0) {
                events.fdReady({source.fd, what});
            }
        }
    }

    void PlatformImpl::dispatchTimeout() {
        if (minDeadline != 0 && monotonicNowUs() >= minDeadline) {
            minDeadline = 0;
            events.timeout();
        }
    }

    void PlatformImpl::run() {
        running = true;
        while (running) {
            @autoreleasepool {
                NSDate* until = [NSDate dateWithTimeIntervalSinceNow:waitSeconds()];
                NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                    untilDate:until
                                                       inMode:NSDefaultRunLoopMode
                                                      dequeue:YES];
                if (event != nil) {
                    [NSApp sendEvent:event];
                }
                dispatchFDs();
                dispatchTimeout();
                events.check();
            }
        }
    }

    void PlatformImpl::stop() {
        running = false;
        NSEvent* event = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                           location:NSZeroPoint
                                      modifierFlags:0
                                          timestamp:0
                                       windowNumber:0
                                            context:nil
                                            subtype:0
                                              data1:0
                                              data2:0];
        [NSApp postEvent:event atStart:NO];
    }

    WindowImpl::WindowImpl(PlatformImpl& platform_, const WindowOptions& options)
        : platform(platform_)
        , input(options.input)
        , events(options.events)
    {
        const NSRect frame = NSMakeRect(0, 0, max(1u, options.width), max(1u, options.height));
        window = [[NSWindow alloc]
            initWithContentRect:frame
                      styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                        backing:NSBackingStoreBuffered
                          defer:NO];
        delegate = [PltWindowDelegate new];
        delegate.owner = this;
        window.delegate = delegate;
        view = [[PltView alloc] initWithFrame:frame];
        view.owner = this;
        view.wantsLayer = YES;
        view.layer = [CAMetalLayer layer];
        window.contentView = view;
        window.acceptsMouseMovedEvents = YES;
        setTitle(options.title);
        setMinimumSize(options.minimumWidth, options.minimumHeight);
        CVDisplayLinkCreateWithActiveCGDisplays(&displayLink);
        if (displayLink != nullptr) {
            CVDisplayLinkSetOutputCallback(displayLink, displayLinkCallback, this);
        }
    }

    WindowImpl::~WindowImpl() {
        cancelFrame();
        if (displayLink != nullptr) {
            CVDisplayLinkRelease(displayLink);
        }
        window.delegate = nil;
        view.owner = nullptr;
        delegate.owner = nullptr;
        [window orderOut:nil];
    }

    void WindowImpl::show() {
        [window center];
        [window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        resized();
    }

    void WindowImpl::requestClose() {
        close();
    }

    bool WindowImpl::requestFrame() {
        if (framePending) {
            return true;
        }
        framePending = true;
        if (displayLink == nullptr || CVDisplayLinkStart(displayLink) != kCVReturnSuccess) {
            framePending = false;
            return false;
        }
        return true;
    }

    void WindowImpl::cancelFrame() {
        framePending = false;
        if (displayLink != nullptr && CVDisplayLinkIsRunning(displayLink)) {
            CVDisplayLinkStop(displayLink);
        }
    }

    void WindowImpl::deliverFrame() {
        if (!framePending) {
            return;
        }
        cancelFrame();
        if (events != nullptr) {
            events->frame();
        }
    }

    void WindowImpl::setTitle(StringView value) {
        NSString* title = stringFromView(value);
        window.title = title == nil ? @"" : title;
    }

    void WindowImpl::requestAttention() {
        [NSApp requestUserAttention:NSInformationalRequest];
    }

    void WindowImpl::requestRedraw() {
        [view setNeedsDisplay:YES];
        if (events != nullptr) {
            events->redraw();
        }
    }

    void WindowImpl::restore() {
        [window deminiaturize:nil];
        if ((window.styleMask & NSWindowStyleMaskFullScreen) != 0) {
            [window toggleFullScreen:nil];
        }
        if (maximized) {
            [window zoom:nil];
            maximized = false;
        }
    }

    void WindowImpl::iconify() {
        [window miniaturize:nil];
    }

    void WindowImpl::move(i32 x, i32 y) {
        NSPoint point = NSMakePoint(x, y);
        [window setFrameOrigin:point];
    }

    void WindowImpl::focus() {
        [window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
    }

    void WindowImpl::setMaximized(bool value) {
        if (maximized != value) {
            [window zoom:nil];
            maximized = value;
        }
    }

    void WindowImpl::setFullscreen(bool value) {
        const bool current = (window.styleMask & NSWindowStyleMaskFullScreen) != 0;
        if (current != value) {
            [window toggleFullScreen:nil];
        }
    }

    void WindowImpl::resize(u32 width, u32 height) {
        const CGFloat scale = window.backingScaleFactor;
        NSSize size = NSMakeSize(max(1u, width) / scale, max(1u, height) / scale);
        [window setContentSize:size];
    }

    void WindowImpl::setMinimumSize(u32 width, u32 height) {
        const CGFloat scale = window.backingScaleFactor;
        window.contentMinSize = NSMakeSize(max(1u, width) / scale, max(1u, height) / scale);
    }

    void WindowImpl::setResizeUnit(u32 width, u32 height, u32, u32) {
        const CGFloat scale = window.backingScaleFactor;
        window.contentResizeIncrements = NSMakeSize(max(1u, width) / scale, max(1u, height) / scale);
    }

    WindowInfo WindowImpl::info() const {
        const NSRect content = [view convertRectToBacking:view.bounds];
        NSScreen* screen = window.screen != nil ? window.screen : [NSScreen mainScreen];
        const NSRect screenFrame = [screen convertRectToBacking:screen.frame];
        return {
            .x = (i32)(window.frame.origin.x),
            .y = (i32)(window.frame.origin.y),
            .width = (u32)(max(1.0, content.size.width)),
            .height = (u32)(max(1.0, content.size.height)),
            .screenPixelWidth = (u32)(max(0.0, screenFrame.size.width)),
            .screenPixelHeight = (u32)(max(0.0, screenFrame.size.height)),
            .contentScale = (float)(window.backingScaleFactor),
            .focused = window.keyWindow,
            .iconified = window.miniaturized,
            .maximized = maximized,
            .fullscreen = (window.styleMask & NSWindowStyleMaskFullScreen) != 0,
        };
    }

    StringView WindowImpl::readPasteboard(NSPasteboard* pasteboard) {
        NSString* value = [pasteboard stringForType:NSPasteboardTypeString];
        text.reset();
        if (value != nil) {
            NSData* data = [value dataUsingEncoding:NSUTF8StringEncoding];
            text.append(data.bytes, data.length);
        }
        return StringView(text);
    }

    void WindowImpl::writePasteboard(NSPasteboard* pasteboard, StringView content) {
        [pasteboard clearContents];
        NSString* value = stringFromView(content);
        [pasteboard setString:value == nil ? @"" : value forType:NSPasteboardTypeString];
    }

    StringView WindowImpl::readPrimary() {
        return readPasteboard([NSPasteboard pasteboardWithName:NSPasteboardNameFind]);
    }

    StringView WindowImpl::readClipboard() {
        return readPasteboard([NSPasteboard generalPasteboard]);
    }

    void WindowImpl::writePrimary(StringView content) {
        writePasteboard([NSPasteboard pasteboardWithName:NSPasteboardNameFind], content);
    }

    void WindowImpl::writeClipboard(StringView content) {
        writePasteboard([NSPasteboard generalPasteboard], content);
    }

    void WindowImpl::pointerIcon(PointerIcon icon) {
        if (icon == PointerIcon::Link) {
            [[NSCursor pointingHandCursor] set];
        } else {
            [[NSCursor IBeamCursor] set];
        }
    }

    RenderContext WindowImpl::renderContext() const {
        return {
            .backend = RenderBackend::Cocoa,
            .connection = nullptr,
            .window = (__bridge void*)(view.layer),
        };
    }

    void WindowImpl::close() {
        if (events != nullptr) {
            events->close();
        }
    }

    void WindowImpl::resized() {
        ((CAMetalLayer*)(view.layer)).contentsScale = window.backingScaleFactor;
        if (events != nullptr) {
            events->resized(info());
        }
    }

    void WindowImpl::focused(bool value) {
        if (input != nullptr) {
            input->focus(value);
        }
    }

    u16 WindowImpl::modifiers(NSEventModifierFlags flags) const {
        u16 result = 0;
        if (flags & NSEventModifierFlagShift) {
            result |= InputShift;
        }
        if (flags & NSEventModifierFlagControl) {
            result |= InputControl;
        }
        if (flags & NSEventModifierFlagOption) {
            result |= InputAlt;
        }
        if (flags & NSEventModifierFlagCommand) {
            result |= InputSuper;
        }
        if (flags & NSEventModifierFlagCapsLock) {
            result |= InputCapsLock;
        }
        if (flags & NSEventModifierFlagNumericPad) {
            result |= InputNumLock;
        }
        return result;
    }

    u32 WindowImpl::firstCodepoint(NSString* string) const {
        if (string.length == 0) {
            return 0;
        }
        const unichar first = [string characterAtIndex:0];
        if (CFStringIsSurrogateHighCharacter(first) && string.length > 1) {
            return CFStringGetLongCharacterForSurrogatePair(first, [string characterAtIndex:1]);
        }
        return first;
    }

    InputKey WindowImpl::inputKey(NSEvent* event) const {
        const u32 value = firstCodepoint(event.charactersIgnoringModifiers);
        switch (value) {
            case 0x1b:
                return InputKey::Escape;
            case '\r':
                return InputKey::Enter;
            case 0x7f:
                return InputKey::Backspace;
            case '\t':
            case NSBackTabCharacter:
                return InputKey::Tab;
            case NSInsertFunctionKey:
                return InputKey::Insert;
            case NSDeleteFunctionKey:
                return InputKey::Delete;
            case NSHomeFunctionKey:
                return InputKey::Home;
            case NSEndFunctionKey:
                return InputKey::End;
            case NSUpArrowFunctionKey:
                return InputKey::Up;
            case NSDownArrowFunctionKey:
                return InputKey::Down;
            case NSLeftArrowFunctionKey:
                return InputKey::Left;
            case NSRightArrowFunctionKey:
                return InputKey::Right;
            case NSPageUpFunctionKey:
                return InputKey::PageUp;
            case NSPageDownFunctionKey:
                return InputKey::PageDown;
            default:
                if (value >= NSF1FunctionKey && value <= NSF20FunctionKey) {
                    return (InputKey)((u8)(InputKey::F1) + value - NSF1FunctionKey);
                }
                return value != 0 ? InputKey::Printable : InputKey::Unknown;
        }
    }

    void WindowImpl::key(NSEvent* event, bool pressed) {
        if (input == nullptr) {
            return;
        }
        const InputAction action = !pressed ? InputAction::Release : (event.isARepeat ? InputAction::Repeat : InputAction::Press);
        const u16 mods = modifiers(event.modifierFlags);
        const u32 layout = firstCodepoint(event.characters);
        const u32 base = firstCodepoint(event.charactersIgnoringModifiers);
        input->key({
            .key = inputKey(event),
            .action = action,
            .modifiers = mods,
            .layoutCodepoint = layout,
            .baseCodepoint = base,
        });
        if (pressed && layout >= 0x20 && layout != 0x7f && !(mods & (InputControl | InputSuper))) {
            input->text({layout, mods});
        }
        input->flush();
    }

    void WindowImpl::flags(NSEvent* event) {
        const u64 changed = modifierState ^ event.modifierFlags;
        modifierState = event.modifierFlags;
        struct ModifierKey {
            u64 flag;
            InputKey key;
        };
        const ModifierKey keys[] = {
            {NSEventModifierFlagShift, InputKey::LeftShift},
            {NSEventModifierFlagControl, InputKey::LeftControl},
            {NSEventModifierFlagOption, InputKey::LeftAlt},
            {NSEventModifierFlagCommand, InputKey::LeftSuper},
            {NSEventModifierFlagCapsLock, InputKey::CapsLock},
        };
        for (const ModifierKey& current : keys) {
            if ((changed & current.flag) && input != nullptr) {
                input->key({
                    .key = current.key,
                    .action = (modifierState & current.flag) ? InputAction::Press : InputAction::Release,
                    .modifiers = modifiers(event.modifierFlags),
                });
            }
        }
        if (input != nullptr) {
            input->flush();
        }
    }

    NSPoint WindowImpl::pointerPosition(NSEvent* event) const {
        NSPoint point = [view convertPoint:event.locationInWindow fromView:nil];
        point.y = view.bounds.size.height - point.y;
        return [view convertPointToBacking:point];
    }

    void WindowImpl::pointer(NSEvent* event) {
        if (input != nullptr) {
            const NSPoint point = pointerPosition(event);
            input->pointerMotion({(int)(point.x), (int)(point.y), modifiers(event.modifierFlags)});
            input->flush();
        }
    }

    void WindowImpl::button(NSEvent* event, bool pressed) {
        if (input == nullptr) {
            return;
        }
        const NSPoint point = pointerPosition(event);
        const i64 number = event.buttonNumber;
        const PointerButton button = number == 0 ? PointerButton::Primary
            : number == 1               ? PointerButton::Secondary
            : number == 2               ? PointerButton::Middle
                                        : (PointerButton)(min<i64>((i64)(PointerButton::Auxiliary5), (i64)(PointerButton::Auxiliary1) + number - 3));
        input->pointerButton({
            .button = button,
            .pressed = pressed,
            .pixelX = (int)(point.x),
            .pixelY = (int)(point.y),
            .modifiers = modifiers(event.modifierFlags),
            .time = event.timestamp,
        });
        input->flush();
    }

    void WindowImpl::scroll(NSEvent* event) {
        if (input != nullptr) {
            const NSPoint point = pointerPosition(event);
            input->scroll({
                .x = event.scrollingDeltaX,
                .y = event.scrollingDeltaY,
                .pixelX = (int)(point.x),
                .pixelY = (int)(point.y),
                .modifiers = modifiers(event.modifierFlags),
            });
            input->flush();
        }
    }

    void WindowImpl::pointerPresence(bool present) {
        if (input != nullptr) {
            input->pointerPresence(present);
            input->flush();
        }
    }

    void cocoaCloseImpl(void* owner) {
        ((WindowImpl*)(owner))->close();
    }

    void cocoaResizeImpl(void* owner) {
        ((WindowImpl*)(owner))->resized();
    }

    void cocoaFocusImpl(void* owner, bool focused) {
        ((WindowImpl*)(owner))->focused(focused);
    }

    void cocoaKeyImpl(void* owner, NSEvent* event, bool pressed) {
        ((WindowImpl*)(owner))->key(event, pressed);
    }

    void cocoaFlagsImpl(void* owner, NSEvent* event) {
        ((WindowImpl*)(owner))->flags(event);
    }

    void cocoaPointerImpl(void* owner, NSEvent* event) {
        ((WindowImpl*)(owner))->pointer(event);
    }

    void cocoaButtonImpl(void* owner, NSEvent* event, bool pressed) {
        ((WindowImpl*)(owner))->button(event, pressed);
    }

    void cocoaScrollImpl(void* owner, NSEvent* event) {
        ((WindowImpl*)(owner))->scroll(event);
    }

    void cocoaPointerPresenceImpl(void* owner, bool present) {
        ((WindowImpl*)(owner))->pointerPresence(present);
    }

    Platform* createNativePlatform(ObjPool& owner, PlatformEvents& events) {
        return owner.make<PlatformImpl>(owner, events);
    }
}
