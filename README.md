# plt

[![CI](https://github.com/pg83/plt/actions/workflows/ci.yml/badge.svg?branch=master)](https://github.com/pg83/plt/actions/workflows/ci.yml)
[![codecov](https://codecov.io/gh/pg83/plt/branch/master/graph/badge.svg)](https://app.codecov.io/gh/pg83/plt/tree/master)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-informational)](STYLE.md)
[![Platforms](https://img.shields.io/badge/platforms-Linux%20%7C%20macOS-8a8a8a)](.github/workflows/ci.yml)

Small native desktop platform layer built on libstd.

The library provides native Linux/Wayland and macOS/Cocoa backends. Its public
surface covers the event loop, generic keyboard and pointer input, clipboard
selections, frame requests and window state. A renderer receives only an
opaque native context; public headers contain no Vulkan, Metal, Wayland or
operating-system headers. Objects are allocated in caller-owned `stl::ObjPool`
instances. The platform owns the event loop; clients register one-shot
file-descriptor callbacks and replaceable timer callbacks through `Poller`.

Window content sizes in `WindowOptions`, `WindowInfo` and `requestResize()`
use buffer pixels. Backends convert to native logical coordinates internally.
Wayland may round a requested size up to a representable logical size.
`contentScale` reports buffer pixels per logical unit.

Build with:

```sh
./build
```
