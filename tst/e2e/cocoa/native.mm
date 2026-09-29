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
@interface DocumentSource: NSView <NSDraggingSource>
@property(nonatomic, strong) id<NSPasteboardWriting> payload;
@end

@implementation DocumentSource

- (void)mouseDown:(NSEvent*)event {
    NSDraggingItem* item = [[NSDraggingItem alloc] initWithPasteboardWriter:self.payload];
    NSImage* image = [[NSImage alloc] initWithSize:NSMakeSize(40, 40)];
    [item setDraggingFrame:NSMakeRect(10, 10, 40, 40) contents:image];
    [self beginDraggingSessionWithItems:@[ item ] event:event source:self];
}

- (NSDragOperation)draggingSession:(NSDraggingSession*)session sourceOperationMaskForDraggingContext:(NSDraggingContext)context {
    (void)session;
    (void)context;
    return NSDragOperationCopy | NSDragOperationMove;
}

@end

bool Canvas::command(const char* value) {
    id<NSTextInputClient> client = (id<NSTextInputClient>)window.contentView;
    if (strcmp(value, "compose") == 0) {
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
        [client setMarkedText:[[NSAttributedString alloc] initWithString:@"日本"] selectedRange:NSMakeRange(2, 0) replacementRange:NSMakeRange(NSNotFound, 0)];
        [client insertText:[[NSAttributedString alloc] initWithString:@"日本🌍"] replacementRange:NSMakeRange(NSNotFound, 0)];
        STD_INSIST(![client hasMarkedText]);
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
        source.contentView = view;
        [source orderFront:nil];
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
