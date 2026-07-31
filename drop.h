#pragma once

#include "clipboard.h"

#include <std/str/view.h>
#include <std/sys/types.h>

namespace stl {
    class Buffer;
    class ObjPool;
}

namespace plt {
    struct InputSink;

    enum class DropAction : u8 {
        None,
        Copy,
        Move
    };

    // The target's answer to a hover: which offered mime it would take and
    // with which action. An empty mime rejects this spot of the window.
    struct DropReply {
        stl::StringView mime;
        DropAction action = DropAction::Copy;
    };

    // Formats the drag source offers. The views stay valid for the duration
    // of the drag session.
    struct DropOffer {
        virtual size_t formats() const = 0;
        virtual stl::StringView format(size_t index) const = 0;
    };

    // A settled drop. At most one read(); the mime must be one of the
    // offered formats and the payload streams through the ClipboardRead
    // contract. The platform completes the session once the read reaches
    // done(). Starting no read before dropped() returns rejects the drop.
    struct Drop {
        virtual DropOffer* what() = 0;
        virtual void read(stl::StringView mime, ClipboardRead& read) = 0;
    };

    struct DropTarget {
        // Called on drag enter and on every motion; x and y are surface
        // pixels. The reply is immediately visible to the drag source.
        virtual DropReply dragOver(const DropOffer& offer, i32 x, i32 y) = 0;
        virtual void dragLeft() = 0;
        virtual void dropped(Drop& drop) = 0;

        // The canonical target: accepts text and uri-list drops with the
        // copy action, buffers one payload whole and delivers it through
        // InputSink::drop or InputSink::dropPath followed by flush().
        static DropTarget* create(stl::ObjPool& owner, InputSink& sink);
    };

    // Iterates one text/uri-list payload: entry receives the next
    // non-comment line and payload shrinks past it. Returns false when no
    // entries remain.
    bool nextUriListEntry(stl::StringView& payload, stl::StringView& entry);
    // Appends the percent-decoded local path of a file:// URI to path.
    // Returns false for other schemes, foreign hosts and malformed escapes;
    // path may then hold a partially decoded prefix.
    bool fileUriToPath(stl::StringView uri, stl::Buffer& path);
}
