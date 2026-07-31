#pragma once

#include <std/str/view.h>

namespace plt {
    struct ClipboardRead {
        // chunk is valid only for the duration of this call. Returning false
        // stops the transfer and completes it with success=false.
        virtual bool data(stl::StringView chunk) = 0;
        // Called exactly once unless the transfer is cancelled.
        virtual void done(bool success) = 0;
    };

    struct Clipboard {
        virtual void read(ClipboardRead& read) = 0;
        virtual void write(stl::StringView content) = 0;
        // After this returns, read receives no more callbacks for cancelled transfers.
        virtual void cancel(ClipboardRead& read) = 0;
    };
}
