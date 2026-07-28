# platform

Small native desktop platform layer built on libstd.

The library provides native Linux/Wayland, macOS/Cocoa and Windows/Win32
backends. Its public surface covers the event loop, generic keyboard and
pointer input, clipboard selections, frame requests and window state. A
renderer receives only an opaque native context; public headers contain no
Vulkan, Metal, Wayland or operating-system headers. Objects are allocated in
caller-owned `stl::ObjPool` instances. The platform owns the event loop;
clients register one-shot file-descriptor callbacks and replaceable timer
callbacks through `Poller`.

Build with:

```sh
./build
```
