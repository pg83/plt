#include "platform_cocoa.h"

#include "input.h"
#include "poller.h"
#include "window.h"
#include "platform.h"

#include <std/sys/crt.h>
#include <std/sym/i_map.h>
#include <std/alg/minmax.h>
#include <std/lib/buffer.h>
#include <std/lib/vector.h>
#include <std/thr/poll_fd.h>
#include <std/mem/obj_pool.h>

#import <AppKit/AppKit.h>
#import <CoreVideo/CoreVideo.h>
#import <IOKit/hidsystem/IOLLEvent.h>
#import <QuartzCore/CAMetalLayer.h>

#include <errno.h>
#include <limits.h>
#include <poll.h>

using namespace stl;
using namespace plt;

namespace plt::cocoa_detail {
    struct Generation {
        u64 advance() {
            ++value;
            if (value == 0) {
                ++value;
            }
            return value;
        }

        bool matches(u64 candidate) const {
            return candidate != 0 && candidate == value;
        }

        u64 value = 0;
    };

    bool readyGenerationMatches(u64 current, u64 ready) {
        return current != 0 && current == ready;
    }

    struct CallbackGate {
        void attach(void* value) {
            __atomic_store_n(&owner_, value, __ATOMIC_RELEASE);
        }

        void detach() {
            __atomic_store_n(&owner_, nullptr, __ATOMIC_RELEASE);
        }

        void* owner() {
            return __atomic_load_n(&owner_, __ATOMIC_ACQUIRE);
        }

        void publish(u64 value) {
            __atomic_store_n(&generation_, value, __ATOMIC_RELEASE);
        }

        u64 generation() {
            return __atomic_load_n(&generation_, __ATOMIC_ACQUIRE);
        }

    private:
        void* owner_ = nullptr;
        u64 generation_ = 0;
    };
}

void cocoaCloseImpl(void* owner);
void cocoaResizeImpl(void* owner);
NSSize cocoaWillResizeImpl(void* owner, NSSize frameSize);
void cocoaFocusImpl(void* owner, bool focused);
void cocoaKeyImpl(void* owner, NSEvent* event, bool pressed);
void cocoaFlagsImpl(void* owner, NSEvent* event);
void cocoaPointerImpl(void* owner, NSEvent* event);
void cocoaButtonImpl(void* owner, NSEvent* event, bool pressed);
void cocoaScrollImpl(void* owner, NSEvent* event);
void cocoaPointerPresenceImpl(void* owner, bool present);

@interface PltWindowDelegate: NSObject <NSWindowDelegate>
@property(nonatomic, assign) void* owner;
@end

@interface PltView: NSView
@property(nonatomic, assign) void* owner;
@property(nonatomic, strong) NSTrackingArea* tracking;
@end

@interface PltDisplayLinkTarget: NSObject {
@public
    cocoa_detail::CallbackGate gate;
}
@end

@implementation PltWindowDelegate

- (BOOL)windowShouldClose:(NSWindow*)sender {
    (void)sender;
    cocoaCloseImpl(self.owner);
    return NO;
}

- (void)windowDidResize:(NSNotification*)notification {
    (void)notification;
    cocoaResizeImpl(self.owner);
}

- (NSSize)windowWillResize:(NSWindow*)sender toSize:(NSSize)frameSize {
    (void)sender;
    return cocoaWillResizeImpl(self.owner, frameSize);
}

- (void)windowDidChangeBackingProperties:(NSNotification*)notification {
    (void)notification;
    cocoaResizeImpl(self.owner);
}

- (void)windowDidBecomeKey:(NSNotification*)notification {
    (void)notification;
    cocoaFocusImpl(self.owner, true);
}

- (void)windowDidResignKey:(NSNotification*)notification {
    (void)notification;
    cocoaFocusImpl(self.owner, false);
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
    self.tracking = [[NSTrackingArea alloc] initWithRect:self.bounds options:NSTrackingMouseEnteredAndExited | NSTrackingMouseMoved | NSTrackingActiveInKeyWindow owner:self userInfo:nil];
    [self addTrackingArea:self.tracking];
    [super updateTrackingAreas];
}

- (void)keyDown:(NSEvent*)event {
    cocoaKeyImpl(self.owner, event, true);
}

- (void)keyUp:(NSEvent*)event {
    cocoaKeyImpl(self.owner, event, false);
}

- (void)flagsChanged:(NSEvent*)event {
    cocoaFlagsImpl(self.owner, event);
}

- (void)mouseMoved:(NSEvent*)event {
    cocoaPointerImpl(self.owner, event);
}

- (void)mouseDragged:(NSEvent*)event {
    cocoaPointerImpl(self.owner, event);
}

- (void)rightMouseDragged:(NSEvent*)event {
    cocoaPointerImpl(self.owner, event);
}

- (void)otherMouseDragged:(NSEvent*)event {
    cocoaPointerImpl(self.owner, event);
}

- (void)mouseDown:(NSEvent*)event {
    cocoaButtonImpl(self.owner, event, true);
}

- (void)mouseUp:(NSEvent*)event {
    cocoaButtonImpl(self.owner, event, false);
}

- (void)rightMouseDown:(NSEvent*)event {
    cocoaButtonImpl(self.owner, event, true);
}

- (void)rightMouseUp:(NSEvent*)event {
    cocoaButtonImpl(self.owner, event, false);
}

- (void)otherMouseDown:(NSEvent*)event {
    cocoaButtonImpl(self.owner, event, true);
}

- (void)otherMouseUp:(NSEvent*)event {
    cocoaButtonImpl(self.owner, event, false);
}

- (void)scrollWheel:(NSEvent*)event {
    cocoaScrollImpl(self.owner, event);
}

- (void)mouseEntered:(NSEvent*)event {
    (void)event;
    cocoaPointerPresenceImpl(self.owner, true);
}

- (void)mouseExited:(NSEvent*)event {
    (void)event;
    cocoaPointerPresenceImpl(self.owner, false);
}

@end

@implementation PltDisplayLinkTarget
@end

namespace {
    struct PlatformImpl;
    struct PollerImpl;
    struct WindowImpl;
    struct ClipboardOperation;

    struct ArmedFD {
        PollFD fd;
        PollCallback* callback = nullptr;
        u64 generation = 0;
    };

    struct ReadyFD {
        PollFD fd;
        u64 generation = 0;
    };

    struct Timer {
        TimerCallback* callback = nullptr;
        u64 deadline = 0;
        u64 generation = 0;
    };

    struct ReadyTimer {
        u64 generation = 0;
    };

    struct PollerImpl final: public Poller {
        explicit PollerImpl(ObjPool& owner);

        void arm(PollFD fd, PollCallback& callback) override;
        void disarm(int fd) override;
        void timeout(u64 microseconds, TimerCallback& callback) override;
        void deadline(u64 monotonicMicroseconds, TimerCallback& callback) override;
        void cancel(TimerCallback& callback) override;

        void dispatch();
        void dispatchTimers();
        u64 nextDeadline() const;

        IntMap<ArmedFD> armed;
        Vector<struct pollfd> pollFDs;
        Vector<ReadyFD> readyFDs;
        Vector<Timer> timers;
        Vector<ReadyTimer> readyTimers;
        cocoa_detail::Generation registrations;
    };

    enum class ClipboardOperationKind : u8 {
        ReadPrimary,
        ReadClipboard,
        WritePrimary,
        WriteClipboard,
    };

    struct ClipboardOperation final: public TimerCallback {
        ClipboardOperation(
            WindowImpl& window,
            ClipboardOperationKind kind,
            ClipboardRead* read,
            StringView content
        );

        void ready() override;
        void cancel();
        void dispose();

        WindowImpl& window;
        ClipboardOperationKind kind;
        ClipboardRead* read = nullptr;
        Buffer content;
        ClipboardOperation* next = nullptr;
        bool timerArmed = true;
        bool dispatching = false;
        bool cancelled = false;
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
        void readPrimary(ClipboardRead& sink) override;
        void readClipboard(ClipboardRead& sink) override;
        void cancelClipboardRead(ClipboardRead& sink) override;
        void writePrimary(StringView content) override;
        void writeClipboard(StringView content) override;
        void pointerIcon(PointerIcon icon) override;
        RenderContext renderContext() const override;

        void close();
        void resized();
        NSSize willResize(NSSize frameSize) const;
        void focused(bool value);
        void key(NSEvent* event, bool pressed);
        void flags(NSEvent* event);
        void pointer(NSEvent* event);
        void button(NSEvent* event, bool pressed);
        void scroll(NSEvent* event);
        void pointerPresence(bool present);
        void deliverFrame(u64 generation);
        u16 modifiers(NSEventModifierFlags flags) const;
        InputKey inputKey(NSEvent* event) const;
        u32 firstCodepoint(NSString* string) const;
        void emitText(NSString* string, u16 modifiers);
        NSPoint pointerPosition(NSEvent* event) const;
        void writePasteboard(NSPasteboard* pasteboard, StringView content);
        void removeClipboardOperation(ClipboardOperation& operation);
        void applySizeConstraints();

        PlatformImpl& platform;
        InputSink* input = nullptr;
        WindowEvents* events = nullptr;
        NSWindow* window = nil;
        PltView* view = nil;
        PltWindowDelegate* delegate = nil;
        CVDisplayLinkRef displayLink = nullptr;
        PltDisplayLinkTarget* displayLinkTarget = nil;
        void* displayLinkContext = nullptr;
        u32 minimumWidth = 1;
        u32 minimumHeight = 1;
        u32 resizeUnitWidth = 1;
        u32 resizeUnitHeight = 1;
        u32 resizeBaseWidth = 0;
        u32 resizeBaseHeight = 0;
        ClipboardOperation* clipboardOperations = nullptr;
        cocoa_detail::Generation frameGeneration;
        bool framePending = false;
    };

    struct PlatformImpl final: public Platform {
        explicit PlatformImpl(ObjPool& owner);

        Window* createWindow(ObjPool& owner, const WindowOptions& options) override;
        Poller* poller() override;
        void run() override;
        void stop() override;

        double waitSeconds() const;

        PollerImpl* poller_ = nullptr;
        bool running = false;
    };

    NSString* stringFromView(StringView value) {
        return [[NSString alloc] initWithBytes:value.data() length:value.length() encoding:NSUTF8StringEncoding];
    }

    CVReturn displayLinkCallback(CVDisplayLinkRef, const CVTimeStamp*, const CVTimeStamp*, CVOptionFlags, CVOptionFlags*, void* context) {
        PltDisplayLinkTarget* const target = (__bridge PltDisplayLinkTarget*)(context);
        const u64 generation = target->gate.generation();
        CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, ^{
          void* const owner = target->gate.owner();
          if (owner != nullptr) {
              ((WindowImpl*)(owner))->deliverFrame(generation);
          }
        });
        CFRunLoopWakeUp(CFRunLoopGetMain());
        return kCVReturnSuccess;
    }
}

PlatformImpl::PlatformImpl(ObjPool& owner)
    : poller_(owner.make<PollerImpl>(owner))
{
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    [NSApp finishLaunching];
}

Window* PlatformImpl::createWindow(ObjPool& owner, const WindowOptions& options) {
    return owner.make<WindowImpl>(*this, options);
}

Poller* PlatformImpl::poller() {
    return poller_;
}

PollerImpl::PollerImpl(ObjPool& owner)
    : armed(ObjPool::create(&owner))
{
}

void PollerImpl::arm(PollFD fd, PollCallback& callback) {
    armed[fd.fd] = {
        .fd = fd,
        .callback = &callback,
        .generation = registrations.advance(),
    };
}

void PollerImpl::disarm(int fd) {
    armed.erase(fd);
}

double PlatformImpl::waitSeconds() const {
    const u64 deadline = poller_->nextDeadline();
    if (deadline == UINT64_MAX) {
        return 0.01;
    }
    const u64 now = monotonicNowUs();
    if (deadline <= now) {
        return 0;
    }
    return min(0.01, (deadline - now) / 1'000'000.0);
}

void PollerImpl::timeout(u64 microseconds, TimerCallback& callback) {
    deadline(monotonicNowUs() + microseconds, callback);
}

void PollerImpl::deadline(u64 monotonicMicroseconds, TimerCallback& callback) {
    if (monotonicMicroseconds == 0) {
        monotonicMicroseconds = monotonicNowUs();
    }
    for (Timer* timer = timers.mutBegin(); timer != timers.mutEnd(); ++timer) {
        if (timer->callback == &callback) {
            timer->deadline = monotonicMicroseconds;
            timer->generation = registrations.advance();
            return;
        }
    }
    timers.pushBack({
        .callback = &callback,
        .deadline = monotonicMicroseconds,
        .generation = registrations.advance(),
    });
}

void PollerImpl::cancel(TimerCallback& callback) {
    for (size_t index = 0; index != timers.length(); ++index) {
        if (timers[index].callback == &callback) {
            timers.mut(index) = timers.back();
            timers.popBack();
            return;
        }
    }
}

u64 PollerImpl::nextDeadline() const {
    u64 result = UINT64_MAX;
    for (const Timer& timer : timers) {
        result = min(result, timer.deadline);
    }
    return result;
}

void PollerImpl::dispatchTimers() {
    const u64 now = monotonicNowUs();
    readyTimers.clear();
    for (size_t index = 0; index != timers.length();) {
        if (timers[index].deadline > now) {
            ++index;
            continue;
        }
        readyTimers.pushBack({.generation = timers[index].generation});
        ++index;
    }
    for (const ReadyTimer& ready : readyTimers) {
        for (size_t index = 0; index != timers.length(); ++index) {
            if (!cocoa_detail::readyGenerationMatches(timers[index].generation, ready.generation)) {
                continue;
            }
            TimerCallback* const callback = timers[index].callback;
            timers.mut(index) = timers.back();
            timers.popBack();
            callback->ready();
            break;
        }
    }
    readyTimers.clear();
}

void PollerImpl::dispatch() {
    pollFDs.clear();
    armed.visit([this](const ArmedFD& source) {
        pollFDs.pushBack({source.fd.fd, source.fd.toPollEvents(), 0});
    });
    readyFDs.clear();
    if (!pollFDs.empty() && ::poll(pollFDs.mutData(), pollFDs.length(), 0) > 0) {
        for (const struct pollfd& source : pollFDs) {
            ArmedFD* registration = armed.find(source.fd);
            if (source.revents == 0 || registration == nullptr) {
                continue;
            }
            readyFDs.pushBack({
                .fd =
                    {
                        .fd = source.fd,
                        .flags = PollFD::fromPollEvents(source.revents),
                    },
                .generation = registration->generation,
            });
        }
    }
    for (const ReadyFD& ready : readyFDs) {
        ArmedFD* registration = armed.find(ready.fd.fd);
        if (registration == nullptr || !cocoa_detail::readyGenerationMatches(registration->generation, ready.generation)) {
            continue;
        }
        PollCallback* const callback = registration->callback;
        armed.erase(ready.fd.fd);
        callback->ready(ready.fd);
    }
    readyFDs.clear();
    dispatchTimers();
}

void PlatformImpl::run() {
    running = true;
    while (running) {
        @autoreleasepool {
            NSDate* until = [NSDate dateWithTimeIntervalSinceNow:waitSeconds()];
            NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:until inMode:NSDefaultRunLoopMode dequeue:YES];
            if (event != nil) {
                [NSApp sendEvent:event];
            }
            poller_->dispatch();
        }
    }
}

void PlatformImpl::stop() {
    running = false;
    NSEvent* event = [NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint modifierFlags:0 timestamp:0 windowNumber:0 context:nil subtype:0 data1:0 data2:0];
    [NSApp postEvent:event atStart:NO];
}

ClipboardOperation::ClipboardOperation(
    WindowImpl& window_,
    ClipboardOperationKind kind_,
    ClipboardRead* read_,
    StringView content_
)
    : window(window_)
    , kind(kind_)
    , read(read_)
    , content(content_)
{
    next = window.clipboardOperations;
    window.clipboardOperations = this;
    window.platform.poller_->timeout(0, *this);
}

void ClipboardOperation::ready() {
    timerArmed = false;
    if (cancelled) {
        dispose();
        return;
    }

    const bool primary =
        kind == ClipboardOperationKind::ReadPrimary
        || kind == ClipboardOperationKind::WritePrimary;
    NSPasteboard* const pasteboard =
        primary
        ? [NSPasteboard pasteboardWithName:NSPasteboardNameFind]
        : [NSPasteboard generalPasteboard];
    if (kind == ClipboardOperationKind::WritePrimary
        || kind == ClipboardOperationKind::WriteClipboard) {
        window.writePasteboard(pasteboard, StringView(content));
        dispose();
        return;
    }

    NSString* value = [pasteboard stringForType:NSPasteboardTypeString];
    NSData* data =
        value == nil
        ? nil
        : [value dataUsingEncoding:NSUTF8StringEncoding];
    bool success = data != nil;
    ClipboardRead* const target = read;
    if (success && data.length != 0) {
        dispatching = true;
        success =
            target != nullptr
            && target->data(
                StringView((const u8*)(data.bytes), data.length)
            );
        dispatching = false;
    }
    if (cancelled) {
        dispose();
        return;
    }

    read = nullptr;
    dispose();
    if (target != nullptr) {
        target->done(success);
    }
}

void ClipboardOperation::cancel() {
    read = nullptr;
    if (dispatching) {
        cancelled = true;
        return;
    }
    dispose();
}

void ClipboardOperation::dispose() {
    if (timerArmed) {
        window.platform.poller_->cancel(*this);
        timerArmed = false;
    }
    window.removeClipboardOperation(*this);
    delete this;
}

WindowImpl::WindowImpl(PlatformImpl& platform_, const WindowOptions& options)
    : platform(platform_)
    , input(options.input)
    , events(options.events)
{
    const NSRect frame = NSMakeRect(0, 0, max(1u, options.width), max(1u, options.height));
    window = [[NSWindow alloc] initWithContentRect:frame styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable backing:NSBackingStoreBuffered defer:NO];
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
        displayLinkTarget = [PltDisplayLinkTarget new];
        displayLinkTarget->gate.attach(this);
        displayLinkContext = (__bridge_retained void*)(displayLinkTarget);
        CVDisplayLinkSetOutputCallback(displayLink, displayLinkCallback, displayLinkContext);
    }
}

WindowImpl::~WindowImpl() {
    while (clipboardOperations != nullptr) {
        clipboardOperations->cancel();
    }
    cancelFrame();
    if (displayLinkTarget != nil) {
        displayLinkTarget->gate.detach();
    }
    if (displayLink != nullptr) {
        CVDisplayLinkRelease(displayLink);
    }
    if (displayLinkContext != nullptr) {
        CFBridgingRelease(displayLinkContext);
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
    if (displayLink == nullptr) {
        return false;
    }
    framePending = true;
    displayLinkTarget->gate.publish(frameGeneration.advance());
    if (CVDisplayLinkStart(displayLink) != kCVReturnSuccess) {
        framePending = false;
        displayLinkTarget->gate.publish(frameGeneration.advance());
        return false;
    }
    return true;
}

void WindowImpl::cancelFrame() {
    framePending = false;
    if (displayLinkTarget != nil) {
        displayLinkTarget->gate.publish(frameGeneration.advance());
    }
    if (displayLink != nullptr && CVDisplayLinkIsRunning(displayLink)) {
        CVDisplayLinkStop(displayLink);
    }
}

void WindowImpl::deliverFrame(u64 generation) {
    if (!framePending || !frameGeneration.matches(generation)) {
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
    if ([window isZoomed]) {
        [window zoom:nil];
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
    if ([window isZoomed] != value) {
        [window zoom:nil];
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
    minimumWidth = max(1u, width);
    minimumHeight = max(1u, height);
    applySizeConstraints();
}

void WindowImpl::setResizeUnit(u32 width, u32 height, u32 baseWidth, u32 baseHeight) {
    resizeUnitWidth = max(1u, width);
    resizeUnitHeight = max(1u, height);
    resizeBaseWidth = baseWidth;
    resizeBaseHeight = baseHeight;
    applySizeConstraints();
}

void WindowImpl::applySizeConstraints() {
    const CGFloat scale = window.backingScaleFactor;
    window.contentMinSize = NSMakeSize(minimumWidth / scale, minimumHeight / scale);
    window.contentResizeIncrements = NSMakeSize(resizeUnitWidth / scale, resizeUnitHeight / scale);
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
        .focused = (bool)(window.keyWindow),
        .iconified = (bool)(window.miniaturized),
        .maximized = (bool)([window isZoomed]),
        .fullscreen = (window.styleMask & NSWindowStyleMaskFullScreen) != 0,
    };
}

void WindowImpl::writePasteboard(NSPasteboard* pasteboard, StringView content) {
    [pasteboard clearContents];
    NSString* value = stringFromView(content);
    [pasteboard setString:value == nil ? @"" : value forType:NSPasteboardTypeString];
}

void WindowImpl::readPrimary(ClipboardRead& sink) {
    new ClipboardOperation(
        *this,
        ClipboardOperationKind::ReadPrimary,
        &sink,
        {}
    );
}

void WindowImpl::readClipboard(ClipboardRead& sink) {
    new ClipboardOperation(
        *this,
        ClipboardOperationKind::ReadClipboard,
        &sink,
        {}
    );
}

void WindowImpl::cancelClipboardRead(ClipboardRead& sink) {
    for (ClipboardOperation* operation = clipboardOperations;
         operation != nullptr;) {
        ClipboardOperation* const next = operation->next;
        if (operation->read == &sink) {
            operation->cancel();
        }
        operation = next;
    }
}

void WindowImpl::writePrimary(StringView content) {
    new ClipboardOperation(
        *this,
        ClipboardOperationKind::WritePrimary,
        nullptr,
        content
    );
}

void WindowImpl::writeClipboard(StringView content) {
    new ClipboardOperation(
        *this,
        ClipboardOperationKind::WriteClipboard,
        nullptr,
        content
    );
}

void WindowImpl::removeClipboardOperation(ClipboardOperation& operation) {
    ClipboardOperation** current = &clipboardOperations;
    while (*current != nullptr) {
        if (*current == &operation) {
            *current = operation.next;
            return;
        }
        current = &(*current)->next;
    }
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
    applySizeConstraints();
    ((CAMetalLayer*)(view.layer)).contentsScale = window.backingScaleFactor;
    if (events != nullptr) {
        events->resized(info());
    }
}

NSSize WindowImpl::willResize(NSSize frameSize) const {
    const NSRect content = [window contentRectForFrameRect:NSMakeRect(0, 0, frameSize.width, frameSize.height)];
    const CGFloat scale = window.backingScaleFactor;
    u32 width = (u32)(max(1.0, content.size.width * scale) + 0.5);
    u32 height = (u32)(max(1.0, content.size.height * scale) + 0.5);
    if (resizeUnitWidth > 1 && width > resizeBaseWidth) {
        width = resizeBaseWidth + ((width - resizeBaseWidth) / resizeUnitWidth) * resizeUnitWidth;
    }
    if (resizeUnitHeight > 1 && height > resizeBaseHeight) {
        height = resizeBaseHeight + ((height - resizeBaseHeight) / resizeUnitHeight) * resizeUnitHeight;
    }
    const NSRect frame = [window frameRectForContentRect:NSMakeRect(0, 0, width / scale, height / scale)];
    return frame.size;
}

void WindowImpl::focused(bool value) {
    if (input != nullptr) {
        input->focus(value);
        input->flush();
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
    if (pressed && !(mods & (InputControl | InputSuper))) {
        emitText(event.characters, mods);
    }
    input->flush();
}

void WindowImpl::emitText(NSString* string, u16 mods) {
    const NSUInteger length = string.length;
    for (NSUInteger index = 0; index < length;) {
        const unichar first = [string characterAtIndex:index++];
        u32 codepoint = first;
        if (CFStringIsSurrogateHighCharacter(first)) {
            if (index == length) {
                continue;
            }
            const unichar second = [string characterAtIndex:index];
            if (!CFStringIsSurrogateLowCharacter(second)) {
                continue;
            }
            ++index;
            codepoint = CFStringGetLongCharacterForSurrogatePair(first, second);
        } else if (CFStringIsSurrogateLowCharacter(first)) {
            continue;
        }
        if (codepoint >= 0x20 && codepoint != 0x7f) {
            input->text({codepoint, mods});
        }
    }
}

void WindowImpl::flags(NSEvent* event) {
    struct ModifierKey {
        u16 keyCode;
        u64 stateFlag;
        u64 otherStateFlag;
        u64 aggregateFlag;
        InputKey key;
    };

    const ModifierKey keys[] = {
        {56, NX_DEVICELSHIFTKEYMASK, NX_DEVICERSHIFTKEYMASK, NSEventModifierFlagShift, InputKey::LeftShift},
        {60, NX_DEVICERSHIFTKEYMASK, NX_DEVICELSHIFTKEYMASK, NSEventModifierFlagShift, InputKey::RightShift},
        {59, NX_DEVICELCTLKEYMASK, NX_DEVICERCTLKEYMASK, NSEventModifierFlagControl, InputKey::LeftControl},
        {62, NX_DEVICERCTLKEYMASK, NX_DEVICELCTLKEYMASK, NSEventModifierFlagControl, InputKey::RightControl},
        {58, NX_DEVICELALTKEYMASK, NX_DEVICERALTKEYMASK, NSEventModifierFlagOption, InputKey::LeftAlt},
        {61, NX_DEVICERALTKEYMASK, NX_DEVICELALTKEYMASK, NSEventModifierFlagOption, InputKey::RightAlt},
        {55, NX_DEVICELCMDKEYMASK, NX_DEVICERCMDKEYMASK, NSEventModifierFlagCommand, InputKey::LeftSuper},
        {54, NX_DEVICERCMDKEYMASK, NX_DEVICELCMDKEYMASK, NSEventModifierFlagCommand, InputKey::RightSuper},
        {57, NSEventModifierFlagCapsLock, 0, NSEventModifierFlagCapsLock, InputKey::CapsLock},
    };
    for (const ModifierKey& current : keys) {
        if (event.keyCode != current.keyCode) {
            continue;
        }
        const bool statePressed = (event.modifierFlags & current.stateFlag) != 0;
        const bool otherStatePressed = (event.modifierFlags & current.otherStateFlag) != 0;
        const bool aggregatePressed = (event.modifierFlags & current.aggregateFlag) != 0;
        const bool pressed = aggregatePressed != (statePressed || otherStatePressed) ? aggregatePressed : statePressed;
        if (input != nullptr) {
            input->key({.key = current.key, .action = pressed ? InputAction::Press : InputAction::Release, .modifiers = modifiers(event.modifierFlags)});
        }
        break;
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
    const PointerButton button = number == 0 ? PointerButton::Primary : number == 1 ? PointerButton::Secondary : number == 2 ? PointerButton::Middle : (PointerButton)(min<i64>((i64)(PointerButton::Auxiliary5), (i64)(PointerButton::Auxiliary1) + number - 3));
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

NSSize cocoaWillResizeImpl(void* owner, NSSize frameSize) {
    return ((WindowImpl*)(owner))->willResize(frameSize);
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

Platform* plt::createCocoaPlatform(ObjPool& owner) {
    return owner.make<PlatformImpl>(owner);
}
