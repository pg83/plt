import os
import platform as host

import build


build.cxxflags += [
    "-std=c++23",
    "-O2",
    "-W",
    "-Wall",
    "-Werror",
]

std_build = os.path.join("third_party", "libstd", "build.py")
if "-lstd" in build.ldflags:
    libstd = dependency(ldflags=["-lstd"])
elif os.path.isfile(os.path.join(os.path.dirname(__file__), std_build)):
    libstd = import_build(std_build, "libstd.a", extra_cflags=["-Wno-error"])
else:
    libstd = dependency(ldflags=["-lstd"])

common_sources = [
    "$(S)/platform/input.cpp",
    "$(S)/platform/platform.cpp",
    "$(S)/platform/window.cpp",
]
target_platform = build.target
if "apple-darwin" in target_platform:
    system = "Darwin"
elif "mingw" in target_platform or "windows" in target_platform:
    system = "Windows"
else:
    system = host.system()

if system == "Linux":
    protocol_names = [
        "viewporter",
        "xdg-shell",
        "fractional-scale-v1",
        "xdg-decoration-unstable-v1",
        "xdg-activation-v1",
        "primary-selection-unstable-v1",
        "cursor-shape-v1",
    ]
    protocol_outputs = []
    protocol_commands = []
    for protocol in protocol_names:
        source = f"$(S)/protocols/{protocol}.xml"
        header = f"$(B)/protocol/{protocol}-client-protocol.h"
        code = f"$(B)/protocol/{protocol}-client-protocol-code.h"
        protocol_outputs += [header, code]
        protocol_commands += [
            ["wayland-scanner", "client-header", source, header],
            ["wayland-scanner", "private-code", source, code],
        ]
    protocols = command(
        inputs=[f"$(S)/protocols/{name}.xml" for name in protocol_names],
        outputs=protocol_outputs,
        cmd=protocol_commands,
        cflags=["-I$(B)/protocol"],
        descr="WL",
        color="blue",
    )
    backend_source = {
        "src": "$(S)/platform/wayland.cpp",
        "inputs": protocol_outputs,
    }
    backend_deps = [
        protocols,
        pkg_config("wayland-client >= 1.20"),
        pkg_config("xkbcommon >= 1.0"),
        dependency(ldflags=["-lrt"]),
    ]
elif system == "Darwin":
    backend_source = "$(S)/platform/cocoa.mm"
    backend_cxxflags = [
        "-fobjc-arc",
        "-fblocks",
        "-Wno-availability",
        "-Wno-missing-method-return-type",
        "-Wno-unused-parameter",
    ]
    backend_deps = [
        dependency(ldflags=[
            "-framework", "AppKit",
            "-framework", "CoreGraphics",
            "-framework", "CoreVideo",
            "-framework", "Metal",
            "-framework", "QuartzCore",
        ]),
    ]
elif system == "Windows":
    backend_source = "$(S)/platform/win32.cpp"
    backend_deps = [
        dependency(ldflags=["-luser32", "-lshell32", "-limm32", "-lole32", "-ldwmapi"]),
    ]
else:
    raise RuntimeError(f"unsupported platform: {system}")

libplatform = library(
    name="platform",
    srcs=[*common_sources, backend_source],
    public_cflags=["-I$(S)"],
    cxxflags=locals().get("backend_cxxflags", []),
    deps=[libstd, *backend_deps],
    output="$(B)/libplatform.a",
)

install(libplatform)
