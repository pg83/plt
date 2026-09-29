#include "native.h"

#include <std/dbg/insist.h>
#include <std/mem/obj_pool.h>

#include <stdio.h>
#include <string.h>

#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

using namespace plt;
using namespace plt::e2e;
using namespace stl;

namespace {
    struct Canvas final: public MetalCanvas {
        explicit Canvas(const RenderContext& context);
        bool paint(const WindowInfo& info, u32 color) override;
        void describe() override;
        bool command(const char* value) override;
        void holdInput(const RenderContext& context) override;
        id<NSTextInputClient> savedInput = nil;
        id dataProvider = nil;
        NSWindow* source = nil;

        CAMetalLayer* layer;
        NSWindow* window;
        id<MTLCommandQueue> queue;
    };
}

Canvas::Canvas(const RenderContext& context)
    : layer((__bridge CAMetalLayer*)context.connection)
    , window((__bridge NSWindow*)context.window)
{
    STD_INSIST(context.backend == RenderBackend::Cocoa);
    layer.device = MTLCreateSystemDefaultDevice();
    STD_INSIST(layer.device != nil);
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.presentsWithTransaction = YES;
    queue = [layer.device newCommandQueue];
    STD_INSIST(queue != nil);
}

bool Canvas::paint(const WindowInfo& info, u32 color) {
    @autoreleasepool {
        layer.drawableSize = CGSizeMake(info.width, info.height);
        id<CAMetalDrawable> drawable = [layer nextDrawable];
        if (drawable == nil) {
            return false;
        }
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = drawable.texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(((color >> 16) & 255) / 255.0, ((color >> 8) & 255) / 255.0, (color & 255) / 255.0, 1);
        id<MTLCommandBuffer> command = [queue commandBuffer];
        id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
        [encoder endEncoding];
        [command commit];
        [command waitUntilCompleted];
        STD_INSIST(command.status == MTLCommandBufferStatusCompleted);
        [drawable present];
        return true;
    }
}

void Canvas::holdInput(const RenderContext& context) {
    savedInput = (id<NSTextInputClient>)((__bridge NSWindow*)context.window).contentView;
}

void Canvas::describe() {
    const NSRect frame = window.frame;
    const CGRect screen = CGDisplayBounds(CGMainDisplayID());
    printf("WINDOW %ld %.0f %.0f %.0f %.0f\n", (long)window.windowNumber, frame.origin.x, screen.size.height - NSMaxY(frame), frame.size.width, frame.size.height);
}

MetalCanvas* MetalCanvas::create(ObjPool& owner, const RenderContext& context) {
    return owner.make<Canvas>(context);
}

// A native drag source alongside the plt client. WindowServer negotiates
// the session with the destination; the driver supplies real mouse events.
@interface LostDocumentProvider: NSObject <NSPasteboardItemDataProvider>
@end

@implementation LostDocumentProvider

- (void)pasteboard:(NSPasteboard*)pasteboard item:(NSPasteboardItem*)item provideDataForType:(NSPasteboardType)type {
    // The source advertised a lazy document which became unavailable.
    (void)pasteboard;
    (void)item;
    (void)type;
}

@end

@interface DocumentSource: NSView <NSDraggingSource>
@property(nonatomic, strong) id<NSPasteboardWriting> payload;
@end

@implementation DocumentSource

- (BOOL)acceptsFirstMouse:(NSEvent*)event {
    (void)event;
    return YES;
}

- (void)mouseDown:(NSEvent*)event {
    puts("SOURCE DRAG BEGIN");
    NSDraggingItem* item = [[NSDraggingItem alloc] initWithPasteboardWriter:self.payload];
    NSImage* image = [[NSImage alloc] initWithSize:NSMakeSize(40, 40)];
    [item setDraggingFrame:NSMakeRect(10, 10, 40, 40) contents:image];
    [self beginDraggingSessionWithItems:@[ item ] event:event source:self];
}

- (void)draggingSession:(NSDraggingSession*)session endedAtPoint:(NSPoint)point operation:(NSDragOperation)operation {
    (void)session;
    (void)point;
    printf("DROP RESULT %lu\n", (unsigned long)operation);
}

- (NSDragOperation)draggingSession:(NSDraggingSession*)session sourceOperationMaskForDraggingContext:(NSDraggingContext)context {
    (void)session;
    (void)context;
    return NSDragOperationCopy | NSDragOperationMove;
}

@end

bool Canvas::command(const char* value) {
    id<NSTextInputClient> client = (id<NSTextInputClient>)window.contentView;
    if (strcmp(value, "native-state") == 0) {
        printf("MINIMIZED %d\n", window.miniaturized);
    } else if (strcmp(value, "native-key-edges") == 0) {
        struct KeyCase {
            NSString* characters;
            NSString* base;
            NSEventModifierFlags flags;
            unsigned short key;
            bool repeated;
        };

        // Event reposting is a public AppKit path used by native input
        // adapters. Keep unusual text/physical-key combinations intact.
        const KeyCase cases[] = {
            {@"a", @"a", 0, 0, true},
            {@"A", @"a", NSEventModifierFlagShift, 0xff, false},
            {@"a", @"é", NSEventModifierFlagShift, 0x40, false},
            {@"\1", @"\1", NSEventModifierFlagOption, 0, false},
        };
        for (const KeyCase& item : cases) {
            const NSEventType types[] = {NSEventTypeKeyDown, NSEventTypeKeyUp};
            for (NSEventType type : types) {
                NSEvent* event = [NSEvent keyEventWithType:type location:NSMakePoint(30, 30) modifierFlags:item.flags timestamp:0 windowNumber:window.windowNumber context:nil characters:item.characters charactersIgnoringModifiers:item.base isARepeat:item.repeated keyCode:item.key];
                [NSApp postEvent:event atStart:NO];
            }
        }
        NSEvent* flags = [NSEvent keyEventWithType:NSEventTypeFlagsChanged location:NSZeroPoint modifierFlags:NSEventModifierFlagShift timestamp:0 windowNumber:window.windowNumber context:nil characters:@"" charactersIgnoringModifiers:@"" isARepeat:NO keyCode:56];
        [NSApp postEvent:flags atStart:NO];
        CGEventRef wheel = CGEventCreateScrollWheelEvent(nullptr, kCGScrollEventUnitLine, 1, 1);
        CGEventSetIntegerValueField(wheel, kCGScrollWheelEventIsContinuous, 0);
        NSEvent* scroll = [NSEvent eventWithCGEvent:wheel];
        [window.contentView scrollWheel:scroll];
        CFRelease(wheel);
    } else if (strcmp(value, "system-close") == 0) {
        [window performClose:nil];
    } else if (strcmp(value, "detached-input") == 0) {
        STD_INSIST(savedInput != nil);
        NSRange actual;
        [savedInput firstRectForCharacterRange:NSMakeRange(0, 0) actualRange:&actual];
        [savedInput unmarkText];
        savedInput = nil;
    } else if (strcmp(value, "mark") == 0) {
        [client setMarkedText:@"にほん" selectedRange:NSMakeRange(1, 1) replacementRange:NSMakeRange(NSNotFound, 0)];
    } else if (strcmp(value, "clear-clipboard") == 0) {
        [[NSPasteboard generalPasteboard] clearContents];
    } else if (strcmp(value, "invalid-text") == 0) {
        const unichar bytes[] = {0xd800, 'A', 0xdc00, 0xdfff, '\n', 0x7f, 0xf700, 0xd800};
        NSString* malformed = [NSString stringWithCharacters:bytes length:sizeof(bytes) / sizeof(*bytes)];
        [client setMarkedText:malformed selectedRange:NSMakeRange(0, 0) replacementRange:NSMakeRange(NSNotFound, 0)];
        [client insertText:malformed replacementRange:NSMakeRange(NSNotFound, 0)];
    } else if (strcmp(value, "gestures") == 0) {
        // Inject real CoreGraphics trackpad events; AppKit constructs NSEvents.
        NSPoint center = [window convertPointToScreen:NSMakePoint(100, 100)];
        CGPoint point = CGPointMake(center.x, CGDisplayBounds(CGMainDisplayID()).size.height - center.y);
        const int phases[] = {128, 1, 2, 4, 8};
        for (int phase : phases) {
            CGEventRef event = CGEventCreateScrollWheelEvent(nullptr, kCGScrollEventUnitPixel, 2, 8, 4);
            CGEventSetLocation(event, point);
            CGEventSetIntegerValueField(event, kCGScrollWheelEventScrollPhase, phase);
            CGEventPost(kCGHIDEventTap, event);
            CFRelease(event);
        }
        CGEventRef momentum = CGEventCreateScrollWheelEvent(nullptr, kCGScrollEventUnitPixel, 1, 8);
        CGEventSetLocation(momentum, point);
        CGEventSetIntegerValueField(momentum, kCGScrollWheelEventMomentumPhase, 1);
        CGEventPost(kCGHIDEventTap, momentum);
        CFRelease(momentum);
    } else if (strcmp(value, "compose") == 0) {
        // Embedded IME client: exercise the public NSTextInputClient contract
        // with marked text, candidate geometry, replacement and cancellation.
        [client setMarkedText:@"にほん" selectedRange:NSMakeRange(1, 1) replacementRange:NSMakeRange(NSNotFound, 0)];
        STD_INSIST([client hasMarkedText]);
        STD_INSIST([client markedRange].length == 3);
        STD_INSIST([client selectedRange].location == 1);
        NSRange actual;
        NSAttributedString* part = [client attributedSubstringForProposedRange:NSMakeRange(1, 1) actualRange:&actual];
        STD_INSIST([part.string isEqualToString:@"ほ"] && actual.location == 1);
        STD_INSIST([client attributedSubstringForProposedRange:NSMakeRange(9, 1) actualRange:nullptr] == nil);
        NSRect caret = [client firstRectForCharacterRange:NSMakeRange(0, 1) actualRange:&actual];
        STD_INSIST(caret.size.height > 0);
        [client characterIndexForPoint:caret.origin];
        [client validAttributesForMarkedText];
        STD_INSIST([client attributedSubstringForProposedRange:NSMakeRange(NSNotFound, 0) actualRange:nullptr] == nil);
        STD_INSIST([client attributedSubstringForProposedRange:NSMakeRange(0, 1) actualRange:nullptr] != nil);
        [client firstRectForCharacterRange:NSMakeRange(0, 1) actualRange:nullptr];
        [client setMarkedText:[[NSAttributedString alloc] initWithString:@"日本"] selectedRange:NSMakeRange(2, 0) replacementRange:NSMakeRange(NSNotFound, 0)];
        [client insertText:[[NSAttributedString alloc] initWithString:@"日本🌍"] replacementRange:NSMakeRange(NSNotFound, 0)];
        STD_INSIST(![client hasMarkedText]);
        STD_INSIST([client attributedSubstringForProposedRange:NSMakeRange(0, 1) actualRange:nullptr] == nil);
        STD_INSIST([client markedRange].location == NSNotFound);
        STD_INSIST([client selectedRange].location == NSNotFound);
        [client setMarkedText:@"" selectedRange:NSMakeRange(0, 0) replacementRange:NSMakeRange(NSNotFound, 0)];
        puts("COMPOSED");
    } else if (strncmp(value, "drag-", 5) == 0) {
        if (source != nil) {
            [source orderOut:nil];
        }
        source = [[NSWindow alloc] initWithContentRect:NSMakeRect(10, 80, 100, 100) styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
        DocumentSource* view = [[DocumentSource alloc] initWithFrame:NSMakeRect(0, 0, 100, 100)];
        view.payload = strcmp(value, "drag-file") == 0 ? (id<NSPasteboardWriting>)[NSURL fileURLWithPath:[NSString stringWithUTF8String:getenv("PLT_DROP_FILE")]] : @"dropped document";
        if (strcmp(value, "drag-both") == 0) {
            NSPasteboardItem* item = [NSPasteboardItem new];
            NSURL* file = [NSURL fileURLWithPath:[NSString stringWithUTF8String:getenv("PLT_DROP_FILE")]];
            [item setString:file.absoluteString forType:NSPasteboardTypeFileURL];
            [item setString:@"dropped document" forType:NSPasteboardTypeString];
            view.payload = item;
        }
        if (strcmp(value, "drag-lost") == 0 || strcmp(value, "drag-lostfile") == 0) {
            NSPasteboardItem* item = [NSPasteboardItem new];
            dataProvider = [LostDocumentProvider new];
            [item setDataProvider:dataProvider forTypes:@[ strcmp(value, "drag-lostfile") == 0 ? NSPasteboardTypeFileURL : NSPasteboardTypeString ]];
            view.payload = item;
        }
        source.contentView = view;
        [source makeKeyAndOrderFront:nil];
        NSPoint point = [source convertPointToScreen:NSMakePoint(30, 50)];
        printf("SOURCE %.0f %.0f\n", point.x, CGDisplayBounds(CGMainDisplayID()).size.height - point.y);
    } else if (strcmp(value, "close-source") == 0) {
        [source orderOut:nil];
        source = nil;
        [window makeKeyAndOrderFront:nil];
    } else {
        return false;
    }
    return true;
}
