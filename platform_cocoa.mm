#include "platform_cocoa.h"

#include "input.h"
#include "poller.h"
#include "window.h"
#include "platform.h"
#include "timer_queue.h"

#include <std/sys/crt.h>
#include <std/dbg/verify.h>
#include <std/sym/i_map.h>
#include <std/alg/minmax.h>
#include <std/lib/buffer.h>
#include <std/thr/poll_fd.h>
#include <std/mem/obj_pool.h>

#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>
#import <CoreVideo/CVDisplayLink.h>
#import <IOKit/hidsystem/IOLLEvent.h>
#import <QuartzCore/CALayer.h>

#include <errno.h>
#include <float.h>
#include <limits.h>
#include <poll.h>

using namespace stl;
using namespace plt;

namespace plt::cocoa_detail {
    struct DisplayLinkGate {
        void attach(void* owner) {
            __atomic_store_n(&owner_, owner, __ATOMIC_RELEASE);
        }

        void detach() {
            __atomic_store_n(&owner_, nullptr, __ATOMIC_RELEASE);
        }

        void* owner() const {
            return __atomic_load_n(&owner_, __ATOMIC_ACQUIRE);
        }

        bool schedule() {
            bool expected = false;
            return __atomic_compare_exchange_n(&scheduled_, &expected, true, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
        }

        void dispatched() {
            __atomic_store_n(&scheduled_, false, __ATOMIC_RELEASE);
        }

        void* owner_ = nullptr;
        bool scheduled_ = false;
    };

}

void cocoaCloseImpl(void* owner);
void cocoaResizeImpl(void* owner);
void cocoaFrameImpl(void* owner);
void cocoaFallbackFrameImpl(void* owner);
void cocoaInvalidateImpl(void* owner);
void cocoaScreenChangedImpl(void* owner);
NSRect cocoaTextInputRectImpl(void* owner);
NSSize cocoaWillResizeImpl(void* owner, NSSize frameSize);
void cocoaFocusImpl(void* owner, bool focused);
void cocoaKeyImpl(void* owner, NSEvent* event, bool pressed);
void cocoaTextImpl(void* owner, NSString* text, NSEventModifierFlags modifiers);
void cocoaPreeditImpl(void* owner, NSString* text);
void cocoaFlushInputImpl(void* owner);
void cocoaFlagsImpl(void* owner, NSEvent* event);
void cocoaPointerImpl(void* owner, NSEvent* event);
void cocoaButtonImpl(void* owner, NSEvent* event, bool pressed);
void cocoaScrollImpl(void* owner, NSEvent* event);
void cocoaPointerPresenceImpl(void* owner, bool present);
void cocoaFileDescriptorReady(CFFileDescriptorRef descriptor, CFOptionFlags types, void* owner);
void cocoaTimerReady(CFRunLoopTimerRef timer, void* owner);

@interface PltWindowDelegate: NSObject <NSWindowDelegate>
@property(nonatomic, assign) void* owner;
@end

@interface PltView: NSView <NSTextInputClient> {
    NSMutableAttributedString* markedText_;
    NSRange selectedTextRange_;
    NSMutableSet<NSNumber*>* composedKeys_;
}
@property(nonatomic, assign) void* owner;
@property(nonatomic, strong) NSTrackingArea* tracking;
@end

@interface PltRootLayer: CALayer
@property(nonatomic, assign) void* owner;
@end

@interface PltDisplayLinkTarget: NSObject {
@public
    plt::cocoa_detail::DisplayLinkGate gate;
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

- (void)windowDidMove:(NSNotification*)notification {
    (void)notification;
    cocoaInvalidateImpl(self.owner);
}

- (void)windowDidChangeScreen:(NSNotification*)notification {
    (void)notification;
    cocoaScreenChangedImpl(self.owner);
}

- (void)windowDidMiniaturize:(NSNotification*)notification {
    (void)notification;
    cocoaInvalidateImpl(self.owner);
}

- (void)windowDidDeminiaturize:(NSNotification*)notification {
    (void)notification;
    cocoaInvalidateImpl(self.owner);
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

- (CALayer*)makeBackingLayer {
    PltRootLayer* layer = [PltRootLayer layer];
    layer.owner = self.owner;
    return layer;
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
    // While the input method composes, the event belongs to the IME:
    // Enter picks a candidate, arrows and Escape navigate the candidate
    // window. Delivering it to the terminal too would double every key.
    // The matching release is swallowed as well: the press was never
    // seen, so an orphan release must not leak (kitty keyboard protocol
    // reports releases).
    if ([self hasMarkedText]) {
        if (composedKeys_ == nil) {
            composedKeys_ = [NSMutableSet set];
        }
        [composedKeys_ addObject:@(event.keyCode)];
    } else {
        cocoaKeyImpl(self.owner, event, true);
    }
    [self interpretKeyEvents:@[ event ]];
    cocoaFlushInputImpl(self.owner);
}

- (void)keyUp:(NSEvent*)event {
    NSNumber* const code = @(event.keyCode);
    if ([composedKeys_ containsObject:code]) {
        [composedKeys_ removeObject:code];
        return;
    }
    cocoaKeyImpl(self.owner, event, false);
    cocoaFlushInputImpl(self.owner);
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

- (void)insertText:(id)value replacementRange:(NSRange)replacementRange {
    (void)replacementRange;
    NSString* const text = [value isKindOfClass:[NSAttributedString class]] ? [value string] : (NSString*)(value);
    [self unmarkText];
    NSEvent* const event = NSApp.currentEvent;
    cocoaTextImpl(self.owner, text, event == nil ? 0 : event.modifierFlags);
}

- (void)doCommandBySelector:(SEL)selector {
    (void)selector;
}

- (BOOL)hasMarkedText {
    return markedText_.length != 0;
}

- (NSRange)markedRange {
    return markedText_.length == 0 ? NSMakeRange(NSNotFound, 0) : NSMakeRange(0, markedText_.length);
}

- (NSRange)selectedRange {
    return markedText_.length == 0 ? NSMakeRange(NSNotFound, 0) : selectedTextRange_;
}

- (void)setMarkedText:(id)value selectedRange:(NSRange)selectedRange replacementRange:(NSRange)replacementRange {
    (void)replacementRange;
    if ([value isKindOfClass:[NSAttributedString class]]) {
        markedText_ = [[NSMutableAttributedString alloc] initWithAttributedString:value];
    } else {
        markedText_ = [[NSMutableAttributedString alloc] initWithString:value];
    }
    selectedTextRange_ = selectedRange;
    if (markedText_.length == 0) {
        [self unmarkText];
        return;
    }
    cocoaPreeditImpl(self.owner, markedText_.string);
}

- (void)unmarkText {
    markedText_ = nil;
    selectedTextRange_ = NSMakeRange(NSNotFound, 0);
    cocoaPreeditImpl(self.owner, nil);
}

- (NSArray<NSAttributedStringKey>*)validAttributesForMarkedText {
    return @[];
}

- (NSAttributedString*)attributedSubstringForProposedRange:(NSRange)range actualRange:(NSRangePointer)actualRange {
    if (markedText_.length == 0 || range.location == NSNotFound || NSMaxRange(range) > markedText_.length) {
        return nil;
    }
    if (actualRange != nullptr) {
        *actualRange = range;
    }
    return [markedText_ attributedSubstringFromRange:range];
}

- (NSUInteger)characterIndexForPoint:(NSPoint)point {
    (void)point;
    return 0;
}

- (NSRect)firstRectForCharacterRange:(NSRange)range actualRange:(NSRangePointer)actualRange {
    if (actualRange != nullptr) {
        *actualRange = range;
    }
    if (self.owner == nullptr) {
        return [self.window convertRectToScreen:NSMakeRect(0, 0, 0, 0)];
    }
    return cocoaTextInputRectImpl(self.owner);
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

@implementation PltRootLayer

- (void)display {
    if (self.owner != nullptr) {
        cocoaFallbackFrameImpl(self.owner);
    }
}

@end

@implementation PltDisplayLinkTarget

// CADisplayLink callback; runs on the main run loop, unlike the CVDisplayLink
// thread callback, so it dispatches directly.
- (void)displayLinkFired:(id)sender {
    (void)sender;
    void* const owner = gate.owner();
    if (owner != nullptr) {
        cocoaFrameImpl(owner);
    }
}

@end

namespace {
    struct PlatformImpl;
    struct PollerImpl;
    struct WindowImpl;
    struct ClipboardOperation;

    struct ArmedFD {
        ArmedFD(PollFD fd, PollCallback* callback, CFFileDescriptorRef descriptor, CFRunLoopSourceRef source);
        ~ArmedFD();

        PollFD fd;
        PollCallback* callback = nullptr;
        CFFileDescriptorRef descriptor = nullptr;
        CFRunLoopSourceRef source = nullptr;
    };

    struct PollerImpl final: public Poller {
        explicit PollerImpl(ObjPool& owner);
        ~PollerImpl();

        void arm(PollFD fd, PollCallback& callback) override;
        void disarm(int fd) override;
        void timeout(u64 microseconds, TimerCallback& callback) override;
        void deadline(u64 monotonicMicroseconds, TimerCallback& callback) override;
        void cancel(TimerCallback& callback) override;

        void descriptorReady(CFFileDescriptorRef descriptor);
        void dispatchTimers();
        void scheduleTimer();
        u64 nextDeadline() const;

        IntMap<ArmedFD> armed;
        TimerQueue timers;
        CFRunLoopTimerRef runLoopTimer = nullptr;
    };

    enum class ClipboardOperationKind : u8 {
        ReadPrimary,
        ReadClipboard,
        WritePrimary,
        WriteClipboard,
    };

    struct ClipboardOperation final: public TimerCallback {
        ClipboardOperation(WindowImpl& window, ClipboardOperationKind kind, ClipboardRead* read, StringView content);

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

        void requestShow() override;
        void requestClose() override;
        void requestFrame() override;
        void requestTitle(StringView title) override;
        void requestAttention() override;
        void requestRestore() override;
        void requestIconify() override;
        void requestMove(i32 x, i32 y) override;
        void requestFocus() override;
        void requestMaximized(bool maximized) override;
        void requestFullscreen(bool fullscreen) override;
        void requestResize(u32 width, u32 height) override;
        void requestMinimumSize(u32 width, u32 height) override;
        void requestResizeUnit(u32 width, u32 height, u32 baseWidth, u32 baseHeight) override;
        WindowInfo info() const override;
        void requestReadPrimary(ClipboardRead& sink) override;
        void requestReadClipboard(ClipboardRead& sink) override;
        void cancelClipboardRead(ClipboardRead& sink) override;
        void requestWritePrimary(StringView content) override;
        void requestWriteClipboard(StringView content) override;
        void requestPointerIcon(PointerIcon icon) override;
        void requestTextInputRect(i32 x, i32 y, u32 width, u32 height) override;
        RenderContext renderContext() const override;

        void close();
        void resized();
        void screenChanged();
        NSRect textInputScreenRect() const;
        void draw();
        void fallbackDraw();
        void stopDisplayLink();
        NSSize willResize(NSSize frameSize) const;
        void focused(bool value);
        void key(NSEvent* event, bool pressed);
        void flushInput();
        void preeditChanged(NSString* text);
        void flags(NSEvent* event);
        void pointer(NSEvent* event);
        void button(NSEvent* event, bool pressed);
        void scroll(NSEvent* event);
        void pointerPresence(bool present);
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
        FrameCallback* frame = nullptr;
        NSWindow* window = nil;
        PltView* view = nil;
        PltWindowDelegate* delegate = nil;
        CVDisplayLinkRef displayLink = nullptr;
        CADisplayLink* caDisplayLink = nil;
        PltDisplayLinkTarget* displayLinkTarget = nil;
        void* displayLinkContext = nullptr;
        i32 textInputX = 0;
        i32 textInputY = 0;
        u32 textInputWidth = 0;
        u32 textInputHeight = 0;
        u32 minimumWidth = 1;
        u32 minimumHeight = 1;
        u32 resizeUnitWidth = 1;
        u32 resizeUnitHeight = 1;
        u32 resizeBaseWidth = 0;
        u32 resizeBaseHeight = 0;
        ClipboardOperation* clipboardOperations = nullptr;
        bool frameRequested = false;
        bool layerFrameRequested = false;
        bool preeditShown = false;
    };

    struct PlatformImpl final: public Platform {
        explicit PlatformImpl(ObjPool& owner);

        Window* createWindow(ObjPool& owner, const WindowOptions& options) override;
        Poller* poller() override;
        void run() override;
        void stop() override;

        PollerImpl* poller_ = nullptr;
    };

    NSString* stringFromView(StringView value) {
        return [[NSString alloc] initWithBytes:value.data() length:value.length() encoding:NSUTF8StringEncoding];
    }

    CVReturn displayLinkCallback(CVDisplayLinkRef, const CVTimeStamp*, const CVTimeStamp*, CVOptionFlags, CVOptionFlags*, void* context) {
        PltDisplayLinkTarget* const target = (__bridge PltDisplayLinkTarget*)(context);
        if (!target->gate.schedule()) {
            return kCVReturnSuccess;
        }
        CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, ^{
          target->gate.dispatched();
          void* const owner = target->gate.owner();
          if (owner != nullptr) {
              cocoaFrameImpl(owner);
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
    , timers(owner)
{
    CFRunLoopTimerContext context{};
    context.info = this;
    runLoopTimer = CFRunLoopTimerCreate(kCFAllocatorDefault, DBL_MAX, 0.000'000'1, 0, 0, cocoaTimerReady, &context);
    STD_VERIFY(runLoopTimer != nullptr);
    CFRunLoopAddTimer(CFRunLoopGetMain(), runLoopTimer, kCFRunLoopCommonModes);
}

PollerImpl::~PollerImpl() {
    CFRunLoopTimerInvalidate(runLoopTimer);
    CFRelease(runLoopTimer);
}

ArmedFD::ArmedFD(PollFD fd_, PollCallback* callback_, CFFileDescriptorRef descriptor_, CFRunLoopSourceRef source_)
    : fd(fd_)
    , callback(callback_)
    , descriptor(descriptor_)
    , source(source_)
{
}

ArmedFD::~ArmedFD() {
    if (source != nullptr) {
        CFRunLoopRemoveSource(CFRunLoopGetMain(), source, kCFRunLoopCommonModes);
        CFRelease(source);
    }
    if (descriptor != nullptr) {
        CFFileDescriptorInvalidate(descriptor);
        CFRelease(descriptor);
    }
}

void PollerImpl::arm(PollFD fd, PollCallback& callback) {
    disarm(fd.fd);
    CFFileDescriptorContext context{};
    context.info = this;
    CFFileDescriptorRef descriptor = CFFileDescriptorCreate(kCFAllocatorDefault, fd.fd, false, cocoaFileDescriptorReady, &context);
    STD_VERIFY(descriptor != nullptr);
    CFRunLoopSourceRef source = CFFileDescriptorCreateRunLoopSource(kCFAllocatorDefault, descriptor, 0);
    STD_VERIFY(source != nullptr);
    armed.insert(fd.fd, fd, &callback, descriptor, source);
    CFRunLoopAddSource(CFRunLoopGetMain(), source, kCFRunLoopCommonModes);
    CFOptionFlags types = 0;
    if (fd.flags & (PollFlag::In | PollFlag::Err | PollFlag::Hup)) {
        types |= kCFFileDescriptorReadCallBack;
    }
    if (fd.flags & PollFlag::Out) {
        types |= kCFFileDescriptorWriteCallBack;
    }
    CFFileDescriptorEnableCallBacks(descriptor, types);
}

void PollerImpl::disarm(int fd) {
    armed.erase(fd);
}

void PollerImpl::timeout(u64 microseconds, TimerCallback& callback) {
    timers.schedule(monotonicNowUs() + microseconds, callback);
    scheduleTimer();
}

void PollerImpl::deadline(u64 monotonicMicroseconds, TimerCallback& callback) {
    if (monotonicMicroseconds == 0) {
        monotonicMicroseconds = monotonicNowUs();
    }
    timers.schedule(monotonicMicroseconds, callback);
    scheduleTimer();
}

void PollerImpl::cancel(TimerCallback& callback) {
    timers.cancel(callback);
    scheduleTimer();
}

u64 PollerImpl::nextDeadline() const {
    return timers.nextDeadline();
}

void PollerImpl::dispatchTimers() {
    timers.dispatch(monotonicNowUs());
    scheduleTimer();
}

void PollerImpl::scheduleTimer() {
    const u64 deadline = nextDeadline();
    if (deadline == UINT64_MAX) {
        CFRunLoopTimerSetNextFireDate(runLoopTimer, DBL_MAX);
        return;
    }
    const u64 now = monotonicNowUs();
    const CFTimeInterval delay = deadline > now ? (deadline - now) / 1'000'000.0 : 0.0;
    CFRunLoopTimerSetNextFireDate(runLoopTimer, CFAbsoluteTimeGetCurrent() + delay);
}

void PollerImpl::descriptorReady(CFFileDescriptorRef descriptor) {
    const int fd = CFFileDescriptorGetNativeDescriptor(descriptor);
    ArmedFD* registration = armed.find(fd);
    if (registration == nullptr || registration->descriptor != descriptor) {
        return;
    }
    struct pollfd event{fd, registration->fd.toPollEvents(), 0};
    const int pollResult = ::poll(&event, 1, 0);
    if (pollResult <= 0 || event.revents == 0) {
        CFOptionFlags types = 0;
        if (registration->fd.flags & (PollFlag::In | PollFlag::Err | PollFlag::Hup)) {
            types |= kCFFileDescriptorReadCallBack;
        }
        if (registration->fd.flags & PollFlag::Out) {
            types |= kCFFileDescriptorWriteCallBack;
        }
        CFFileDescriptorEnableCallBacks(descriptor, types);
        return;
    }
    PollCallback* const callback = registration->callback;
    PollFD ready{
        .fd = fd,
        .flags = PollFD::fromPollEvents(event.revents),
    };
    armed.erase(fd);
    callback->ready(ready);
    NSEvent* wakeup = [NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint modifierFlags:0 timestamp:0 windowNumber:0 context:nil subtype:0 data1:0 data2:0];
    [NSApp postEvent:wakeup atStart:NO];
}

void PlatformImpl::run() {
    [NSApp run];
}

void PlatformImpl::stop() {
    [NSApp stop:nil];
    NSEvent* event = [NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint modifierFlags:0 timestamp:0 windowNumber:0 context:nil subtype:0 data1:0 data2:0];
    [NSApp postEvent:event atStart:NO];
}

ClipboardOperation::ClipboardOperation(WindowImpl& window_, ClipboardOperationKind kind_, ClipboardRead* read_, StringView content_)
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

    const bool primary = kind == ClipboardOperationKind::ReadPrimary || kind == ClipboardOperationKind::WritePrimary;
    NSPasteboard* const pasteboard = primary ? [NSPasteboard pasteboardWithName:NSPasteboardNameFind] : [NSPasteboard generalPasteboard];
    if (kind == ClipboardOperationKind::WritePrimary || kind == ClipboardOperationKind::WriteClipboard) {
        window.writePasteboard(pasteboard, StringView(content));
        dispose();
        return;
    }

    NSString* value = [pasteboard stringForType:NSPasteboardTypeString];
    NSData* data = value == nil ? nil : [value dataUsingEncoding:NSUTF8StringEncoding];
    bool success = data != nil;
    ClipboardRead* const target = read;
    if (success && data.length != 0) {
        dispatching = true;
        success = target != nullptr && target->data(StringView((const u8*)(data.bytes), data.length));
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
    , frame(options.frame)
{
    const NSRect frame = NSMakeRect(0, 0, max(1u, options.width), max(1u, options.height));
    window = [[NSWindow alloc] initWithContentRect:frame styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable backing:NSBackingStoreBuffered defer:NO];
    delegate = [PltWindowDelegate new];
    delegate.owner = this;
    window.delegate = delegate;
    view = [[PltView alloc] initWithFrame:frame];
    view.owner = this;
    view.wantsLayer = YES;
    view.layerContentsRedrawPolicy = NSViewLayerContentsRedrawDuringViewResize;
    window.contentView = view;
    window.acceptsMouseMovedEvents = YES;
    requestTitle(options.title);
    requestMinimumSize(options.minimumWidth, options.minimumHeight);
    // Prefer the view display link: it runs on the main run loop and follows
    // the view across displays by itself. CVDisplayLink stays as the fallback
    // for older systems and needs manual rebinding on screen changes.
    if (@available(macOS 14.0, *)) {
        displayLinkTarget = [PltDisplayLinkTarget new];
        displayLinkTarget->gate.attach(this);
        caDisplayLink = [view displayLinkWithTarget:displayLinkTarget selector:@selector(displayLinkFired:)];
        if (caDisplayLink != nil) {
            caDisplayLink.paused = YES;
            [caDisplayLink addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
        } else {
            displayLinkTarget->gate.detach();
            displayLinkTarget = nil;
        }
    }
    if (caDisplayLink == nil && CVDisplayLinkCreateWithActiveCGDisplays(&displayLink) == kCVReturnSuccess && displayLink != nullptr) {
        displayLinkTarget = [PltDisplayLinkTarget new];
        displayLinkTarget->gate.attach(this);
        displayLinkContext = (__bridge_retained void*)(displayLinkTarget);
        if (CVDisplayLinkSetOutputCallback(displayLink, displayLinkCallback, displayLinkContext) != kCVReturnSuccess) {
            displayLinkTarget->gate.detach();
            CFBridgingRelease(displayLinkContext);
            displayLinkContext = nullptr;
            displayLinkTarget = nil;
            CVDisplayLinkRelease(displayLink);
            displayLink = nullptr;
        }
    }
}

WindowImpl::~WindowImpl() {
    while (clipboardOperations != nullptr) {
        clipboardOperations->cancel();
    }
    if (displayLinkTarget != nil) {
        displayLinkTarget->gate.detach();
    }
    stopDisplayLink();
    if (caDisplayLink != nil) {
        [caDisplayLink invalidate];
        caDisplayLink = nil;
    }
    if (displayLink != nullptr) {
        CVDisplayLinkRelease(displayLink);
    }
    if (displayLinkContext != nullptr) {
        CFBridgingRelease(displayLinkContext);
    }
    window.delegate = nil;
    view.owner = nullptr;
    ((PltRootLayer*)(view.layer)).owner = nullptr;
    delegate.owner = nullptr;
    [window orderOut:nil];
}

void WindowImpl::requestShow() {
    [window center];
    [window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    resized();
}

void WindowImpl::requestClose() {
    close();
}

void WindowImpl::requestFrame() {
    if (frameRequested) {
        return;
    }
    frameRequested = true;
    if (caDisplayLink != nil) {
        caDisplayLink.paused = NO;
        return;
    }
    if (displayLink != nullptr) {
        if (!CVDisplayLinkIsRunning(displayLink) && CVDisplayLinkStart(displayLink) == kCVReturnSuccess) {
            return;
        }
        if (CVDisplayLinkIsRunning(displayLink)) {
            return;
        }
    }
    layerFrameRequested = true;
    [view.layer setNeedsDisplay];
}

void WindowImpl::draw() {
    if (!frameRequested || frame == nullptr) {
        stopDisplayLink();
        return;
    }
    frameRequested = false;
    frame->frame(info());
    if (!frameRequested) {
        stopDisplayLink();
    }
}

void WindowImpl::fallbackDraw() {
    if (!layerFrameRequested) {
        return;
    }
    layerFrameRequested = false;
    draw();
}

void WindowImpl::stopDisplayLink() {
    if (caDisplayLink != nil) {
        caDisplayLink.paused = YES;
    }
    if (displayLink != nullptr && CVDisplayLinkIsRunning(displayLink)) {
        CVDisplayLinkStop(displayLink);
    }
}

void WindowImpl::requestTitle(StringView value) {
    NSString* title = stringFromView(value);
    window.title = title == nil ? @"" : title;
}

void WindowImpl::requestAttention() {
    [NSApp requestUserAttention:NSInformationalRequest];
}

void WindowImpl::requestRestore() {
    [window deminiaturize:nil];
    if ((window.styleMask & NSWindowStyleMaskFullScreen) != 0) {
        [window toggleFullScreen:nil];
    }
    if ([window isZoomed]) {
        [window zoom:nil];
    }
}

void WindowImpl::requestIconify() {
    [window miniaturize:nil];
}

void WindowImpl::requestMove(i32 x, i32 y) {
    NSPoint point = NSMakePoint(x, y);
    [window setFrameOrigin:point];
}

void WindowImpl::requestFocus() {
    [window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

void WindowImpl::requestMaximized(bool value) {
    if ([window isZoomed] != value) {
        [window zoom:nil];
    }
}

void WindowImpl::requestFullscreen(bool value) {
    const bool current = (window.styleMask & NSWindowStyleMaskFullScreen) != 0;
    if (current != value) {
        [window toggleFullScreen:nil];
    }
}

void WindowImpl::requestResize(u32 width, u32 height) {
    const CGFloat scale = window.backingScaleFactor;
    NSSize size = NSMakeSize(max(1u, width) / scale, max(1u, height) / scale);
    [window setContentSize:size];
}

void WindowImpl::requestMinimumSize(u32 width, u32 height) {
    minimumWidth = max(1u, width);
    minimumHeight = max(1u, height);
    applySizeConstraints();
}

void WindowImpl::requestResizeUnit(u32 width, u32 height, u32 baseWidth, u32 baseHeight) {
    resizeUnitWidth = max(1u, width);
    resizeUnitHeight = max(1u, height);
    resizeBaseWidth = baseWidth;
    resizeBaseHeight = baseHeight;
}

void WindowImpl::applySizeConstraints() {
    const CGFloat scale = window.backingScaleFactor;
    window.contentMinSize = NSMakeSize(minimumWidth / scale, minimumHeight / scale);
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

void WindowImpl::requestReadPrimary(ClipboardRead& sink) {
    new ClipboardOperation(*this, ClipboardOperationKind::ReadPrimary, &sink, {});
}

void WindowImpl::requestReadClipboard(ClipboardRead& sink) {
    new ClipboardOperation(*this, ClipboardOperationKind::ReadClipboard, &sink, {});
}

void WindowImpl::cancelClipboardRead(ClipboardRead& sink) {
    for (ClipboardOperation* operation = clipboardOperations; operation != nullptr;) {
        ClipboardOperation* const next = operation->next;
        if (operation->read == &sink) {
            operation->cancel();
        }
        operation = next;
    }
}

void WindowImpl::requestWritePrimary(StringView content) {
    new ClipboardOperation(*this, ClipboardOperationKind::WritePrimary, nullptr, content);
}

void WindowImpl::requestWriteClipboard(StringView content) {
    new ClipboardOperation(*this, ClipboardOperationKind::WriteClipboard, nullptr, content);
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

void WindowImpl::requestPointerIcon(PointerIcon icon) {
    if (icon == PointerIcon::Link) {
        [[NSCursor pointingHandCursor] set];
    } else {
        [[NSCursor IBeamCursor] set];
    }
}

RenderContext WindowImpl::renderContext() const {
    PltRootLayer* const layer = (PltRootLayer*)(view.layer);
    return {
        .backend = RenderBackend::Cocoa,
        .connection = (__bridge void*)(layer),
        .window = nullptr,
    };
}

void WindowImpl::close() {
    if (events != nullptr) {
        events->close();
    }
}

void WindowImpl::resized() {
    applySizeConstraints();
    PltRootLayer* const layer = (PltRootLayer*)(view.layer);
    layer.contentsScale = window.backingScaleFactor;
    requestFrame();
    layerFrameRequested = true;
    [view.layer setNeedsDisplay];
}

void WindowImpl::screenChanged() {
    // The CADisplayLink from NSView tracks the view's display by itself; the
    // CVDisplayLink fallback must be retargeted or it keeps pacing frames at
    // the previous display's refresh rate.
    if (displayLink != nullptr) {
        NSScreen* const screen = window.screen;
        NSNumber* const number = screen == nil ? nil : screen.deviceDescription[@"NSScreenNumber"];
        if (number != nil) {
            CVDisplayLinkSetCurrentCGDisplay(displayLink, (CGDirectDisplayID)(number.unsignedIntValue));
        }
    }
    requestFrame();
}

void WindowImpl::requestTextInputRect(i32 x, i32 y, u32 width, u32 height) {
    textInputX = x;
    textInputY = y;
    textInputWidth = width;
    textInputHeight = height;
}

NSRect WindowImpl::textInputScreenRect() const {
    const CGFloat scale = window.backingScaleFactor;
    NSRect rect = NSMakeRect(textInputX / scale, view.bounds.size.height - (textInputY + (CGFloat)(max(1u, textInputHeight))) / scale, max(1u, textInputWidth) / scale, max(1u, textInputHeight) / scale);
    rect = [view convertRect:rect toView:nil];
    return [window convertRectToScreen:rect];
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
    requestFrame();
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
    switch (event.keyCode) {
        case kVK_ANSI_Keypad0:
            return InputKey::Keypad0;
        case kVK_ANSI_Keypad1:
            return InputKey::Keypad1;
        case kVK_ANSI_Keypad2:
            return InputKey::Keypad2;
        case kVK_ANSI_Keypad3:
            return InputKey::Keypad3;
        case kVK_ANSI_Keypad4:
            return InputKey::Keypad4;
        case kVK_ANSI_Keypad5:
            return InputKey::Keypad5;
        case kVK_ANSI_Keypad6:
            return InputKey::Keypad6;
        case kVK_ANSI_Keypad7:
            return InputKey::Keypad7;
        case kVK_ANSI_Keypad8:
            return InputKey::Keypad8;
        case kVK_ANSI_Keypad9:
            return InputKey::Keypad9;
        case kVK_ANSI_KeypadDecimal:
            return InputKey::KeypadDecimal;
        case kVK_ANSI_KeypadDivide:
            return InputKey::KeypadDivide;
        case kVK_ANSI_KeypadMultiply:
            return InputKey::KeypadMultiply;
        case kVK_ANSI_KeypadMinus:
            return InputKey::KeypadSubtract;
        case kVK_ANSI_KeypadPlus:
            return InputKey::KeypadAdd;
        case kVK_ANSI_KeypadEnter:
            return InputKey::KeypadEnter;
        case kVK_ANSI_KeypadEquals:
            return InputKey::KeypadEqual;
        case kVK_ANSI_KeypadClear:
            return InputKey::NumLock;
        default:
            break;
    }
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
            if (value >= NSF1FunctionKey && value <= NSF35FunctionKey) {
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
    u16 mods = modifiers(event.modifierFlags);
    const InputKey key = inputKey(event);
    if (key >= InputKey::Keypad0 && key <= InputKey::KeypadDecimal) {
        mods |= InputNumLock;
    }
    const u32 layout = firstCodepoint(event.characters);
    const u32 base = firstCodepoint(event.charactersIgnoringModifiers);
    input->key({
        .key = key,
        .action = action,
        .modifiers = mods,
        .layoutCodepoint = layout,
        .baseCodepoint = base,
    });
}

void WindowImpl::flushInput() {
    if (input != nullptr) {
        input->flush();
    }
}

void WindowImpl::preeditChanged(NSString* text) {
    if (input == nullptr) {
        return;
    }
    if (text == nil || text.length == 0) {
        if (preeditShown) {
            input->preedit({}, -1, -1);
            input->flush();
            preeditShown = false;
        }
        return;
    }
    NSData* const data = [text dataUsingEncoding:NSUTF8StringEncoding];
    if (data == nil) {
        return;
    }
    input->preedit(StringView((const u8*)(data.bytes), data.length), -1, -1);
    input->flush();
    preeditShown = true;
}

void WindowImpl::emitText(NSString* string, u16 mods) {
    if (input == nullptr || (mods & (InputControl | InputSuper))) {
        return;
    }
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
        if (codepoint >= 0x20 && codepoint != 0x7f && !(codepoint >= 0xf700 && codepoint <= 0xf7ff)) {
            input->text({codepoint, mods});
        }
    }
    input->flush();
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
        const double scale = event.hasPreciseScrollingDeltas ? 0.1 : 1.0;
        input->scroll({
            .x = event.scrollingDeltaX * scale,
            .y = event.scrollingDeltaY * scale,
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

void cocoaFrameImpl(void* owner) {
    ((WindowImpl*)(owner))->draw();
}

void cocoaFallbackFrameImpl(void* owner) {
    ((WindowImpl*)(owner))->fallbackDraw();
}

void cocoaInvalidateImpl(void* owner) {
    ((WindowImpl*)(owner))->requestFrame();
}

void cocoaScreenChangedImpl(void* owner) {
    ((WindowImpl*)(owner))->screenChanged();
}

NSRect cocoaTextInputRectImpl(void* owner) {
    return ((WindowImpl*)(owner))->textInputScreenRect();
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

void cocoaTextImpl(void* owner, NSString* text, NSEventModifierFlags flags) {
    WindowImpl* const window = (WindowImpl*)(owner);
    window->emitText(text, window->modifiers(flags));
}

void cocoaFlushInputImpl(void* owner) {
    ((WindowImpl*)(owner))->flushInput();
}

void cocoaPreeditImpl(void* owner, NSString* text) {
    if (owner != nullptr) {
        ((WindowImpl*)(owner))->preeditChanged(text);
    }
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

void cocoaFileDescriptorReady(CFFileDescriptorRef descriptor, CFOptionFlags types, void* owner) {
    (void)types;
    ((PollerImpl*)(owner))->descriptorReady(descriptor);
}

void cocoaTimerReady(CFRunLoopTimerRef, void* owner) {
    ((PollerImpl*)(owner))->dispatchTimers();
}

Platform* plt::createCocoaPlatform(ObjPool& owner) {
    return owner.make<PlatformImpl>(owner);
}
