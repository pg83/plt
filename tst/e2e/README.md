# Real compositor scenarios

Every scenario is a C++ application using plt's public API and a paired Python
script. The driver configures the application through environment variables.
Linux scenarios start an isolated headless Sway compositor, inject real keyboard
and pointer input, and check pixels captured by grim. Cocoa scenarios use the
runner's WindowServer, Metal, CoreGraphics input, and screencapture. Missing tools and timeouts fail the
run. Each scenario preserves client/compositor logs, PNG screenshots and Sway
window geometry; the runner writes `results.json` and continues after failures.

Build and run with libstd configured as for the ordinary library build:

```sh
./build e2e-binaries
./build e2e
python3 tst/e2e/run.py --binary-dir .build/e2e \
    --artifacts .build/e2e-lavapipe --renderer lavapipe
```

`e2e` runs the shm path. The lavapipe path creates a real Wayland Vulkan surface
and swapchain and requires a CPU Vulkan device. Set `VK_DRIVER_FILES` to Mesa's
`lvp_icd*.json` and `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation` to reproduce
CI. Vulkan validation errors fail the scenario. Use `--filter gallery` to run
one pair.

Build dependencies additionally include Cairo and Vulkan development packages.
Runtime tools: Sway, swaymsg, swaybg, grim, wtype, wl-copy and wl-paste, plus
DejaVu fonts. Lavapipe needs Mesa Vulkan drivers and Vulkan validation layers.
Run as a regular user with writable temporary storage and shared memory.
`dev/ci_linux.sh` installs the CI dependencies.

| Program | Scenario |
| --- | --- |
| gallery | Presentation, resizing, fractional and integer output scales |
| native_map | Native toolkit maps the exposed Wayland surface; plt handles subsequent resize and scale changes |
| native_preview | A native toolkit and plt share one Wayland connection, with separate input focus |
| editor | Text entry and deletion |
| connection_warmup | Writable-only display recovery before the first document opens |
| compose_focus | Replaced and stale IME focus events, preedit cleanup and isolated document input |
| drop_import | Real copy/move negotiation, rejection, interrupted transfers and window retirement during hover |
| drop_lifetime | Cross-process drag enter queued before its target surface is destroyed |
| drop_shutdown | Shutdown and replacement while a file validator keeps previous drag callbacks suspended |
| pointer | Clicking, dragging and pointer icons |
| scroller | Wheel input and repainting |
| clipboard | Streaming copy/paste with independent clipboard clients |
| primary | Primary selection exchange through mouse input |
| windows | Multiple windows and focus changes |
| window_state | Fullscreen, resize, title and compositor close |
| animation | Timers and repeated frame requests |
| pipe_dashboard | Fiber waiting for external FIFO data |
| worker | Background thread progress and event loop wakeups |
| render_retry | Retrying deferred rendering |
| file_transfer | Two concurrent document senders, mutex contention, socket backpressure, timeout and exact output bytes |
| layout_preview | Headless rendering, resize/fullscreen layouts, failed presentation retry, exported pixels and desktop preview |
| cocoa/desktop | Native Metal window, external keyboard/mouse, cursor shapes, clipboard, resizing and window state |

Every Linux test job (GCC/glibc, Clang/musl, Clang/ASan, Clang/UBSan and
coverage) runs both renderers, using the same compiler and instrumentation as
the Wayland integration tests. All jobs are gated by `build`. Linux coverage includes their production code coverage
in the report merged with Darwin. Screenshots and logs are uploaded on success
and failure.

## macOS

The Darwin test and coverage jobs also build and run the Cocoa desktop program.
The Python driver posts keyboard input to the application, mouse input through
WindowServer, exchanges Unicode text using pbcopy/pbpaste, and checks captured
window pixels through minimize/restore, maximize and fullscreen transitions.
The renderer uses real Metal drawables. Missing graphical capabilities fail the
scenario; there is no silent skip.

```sh
./build e2e-binaries
python3 tst/e2e/run.py --binary-dir .build/e2e \
    --source-dir tst/e2e/cocoa --renderer metal --artifacts .build/e2e-metal
```

The macOS 14 coverage job exercises the cursor fallbacks on their actual OS.
GitHub [retires this runner on November 2, 2026](https://github.com/actions/runner-images/issues/13518);
retaining the compatibility test after that date requires another macOS 14 runner.
The desktop changes the monitor ICC profile through ColorSync, waits for the
system backing-property notification, restores the saved custom profiles, and
checks the rendered image. This reaches the Cocoa backing-property callback on
runners whose displays expose only a 1x scale, without invoking the delegate
or posting its notification from the test.
The `backing-mode` probe records every advertised display mode, uses a different
backing scale when the runner exposes one, and restores the original mode.

## Coverage

The merged Linux/Darwin report includes all compiled production objects in
`libplt.a`, including code no test executable links. The CI coverage job requires
**100% line coverage and 100% branch coverage** across the library.
This is coverage from the complete test suite (Wayland integration, Cocoa checks
and e2e). Dependencies, drivers and sample programs do not count as production
code. CI saves the merged tracefile and summary even when a threshold fails;
a passing coverage job requires both thresholds.

## Production and test archives

`./build plt` produces the installed production `libplt.a`.
`./build plt_test` produces `libplt_test.a` with `PLATFORM_FOR_TESTS=1`;
all integration and e2e programs link this archive. Both implementations live in `chaos_monkey.cpp`: `PLATFORM_FOR_TESTS`
selects the fault script, and the production branch is inert. CI checks both archives for the presence/absence of the
`PLT_CHAOS` control string. Coverage exports both full archives, so production
objects not linked by any test still count in the denominator. The test-only
parser is not instrumented and does not count toward library coverage.

The deterministic fault script is passed to each client through its environment:

```sh
PLT_CHAOS='selection-pipe@0,read-interrupted@1' path/to/client
```

Each `name@skip` rule lets `skip` matching calls pass, then fails the next call
once. Rules are independent and reset for each process. A fired rule logs
`CHAOS name`; drivers assert that the requested fault actually fired and that
the client subsequently recovers. Invalid and duplicate rules fail immediately.
Faults are injected before resource creation or I/O, preserving OS ownership
rules and preventing successful resources from being leaked by replacement.

Available points:

- Wayland: `keymap-map`, `keymap-compile`, `keymap-state`, `compose-table`,
  `selection-pipe`, `selection-flags`, `selection-read`, `selection-write`,
  `read-interrupted`, `write-interrupted`.
- Cocoa: `display-link`, `display-callback`, `poll-interrupted`.

`fault_recovery` uses real clipboard peers under Sway and checks failed/recovered
frames. Native `recovery` checks drawing and input after display-link setup
failure or interrupted descriptor polling. The desktop scenario also operates
as an embedded input-method client and imports text/files from a separate native
drag-source window via WindowServer. `async_import`/`importer` import a real file
from a background worker, verify its complete length and checksum, and cancel
obsolete requests while descriptors and deadlines remain pending.
