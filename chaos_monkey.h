#pragma once

namespace plt {
    // Faults are spent before the system call: no successful resource is
    // discarded and ownership remains exactly as on a real failure.
    enum class Fault {
        KeymapMap,
        KeymapCompile,
        KeymapState,
        ComposeTable,
        SelectionPipe,
        SelectionFlags,
        SelectionRead,
        SelectionWrite,
        ReadInterrupted,
        WriteInterrupted,
        DisplayLink,
        DisplayCallback,
        PollInterrupted,
        Count,
    };

    bool chaos(Fault fault);
}
