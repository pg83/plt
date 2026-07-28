import platform as host

import build


build.cxxflags += [
    "-std=c++23",
    "-O2",
    "-W",
    "-Wall",
    "-Werror",
]

libstd = dependency(
    ldflags=[]
    if "-Dno_vendored_std" in build.cppflags or "-lstd" in build.ldflags
    else ["-lstd"]
)

common_sources = [
    "$(S)/input.cpp",
    "$(S)/poller.cpp",
    "$(S)/pointer_grab.cpp",
    "$(S)/platform.cpp",
    "$(S)/window.cpp",
]
target_platform = build.target
if "apple-darwin" in target_platform:
    system = "Darwin"
elif "mingw" in target_platform or "windows" in target_platform:
    system = "Windows"
else:
    system = host.system()

if system == "Linux":
    protocol_root = pkg_config_variable("wayland-protocols", "pkgdatadir")
    protocol_paths = [
        "stable/viewporter/viewporter",
        "stable/xdg-shell/xdg-shell",
        "staging/fractional-scale/fractional-scale-v1",
        "unstable/xdg-decoration/xdg-decoration-unstable-v1",
        "staging/xdg-activation/xdg-activation-v1",
        "unstable/primary-selection/primary-selection-unstable-v1",
        "staging/cursor-shape/cursor-shape-v1",
    ]
    protocol_outputs = []
    protocol_commands = []
    for path in protocol_paths:
        protocol = path.rsplit("/", 1)[-1]
        source = f"{protocol_root}/{path}.xml"
        header = f"$(B)/protocol/{protocol}-client-protocol.h"
        code = f"$(B)/protocol/{protocol}-client-protocol-code.h"
        protocol_outputs += [header, code]
        protocol_commands += [
            ["wayland-scanner", "client-header", source, header],
            ["wayland-scanner", "private-code", source, code],
        ]
    protocols = command(
        inputs=[f"{protocol_root}/{path}.xml" for path in protocol_paths],
        outputs=protocol_outputs,
        cmd=protocol_commands,
        cflags=["-I$(B)/protocol"],
        descr="WL",
        color="blue",
    )
    backend_source = {
        "src": "$(S)/platform_wayland.cpp",
        "inputs": protocol_outputs,
    }
    backend_deps = [
        protocols,
        pkg_config("wayland-client >= 1.20"),
        pkg_config("xkbcommon >= 1.0"),
        dependency(ldflags=["-lrt"]),
    ]
elif system == "Darwin":
    backend_source = "$(S)/platform_cocoa.mm"
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
    backend_source = "$(S)/platform_win32.cpp"
    backend_deps = [
        dependency(ldflags=["-luser32", "-lshell32", "-limm32", "-lole32", "-ldwmapi"]),
    ]
else:
    raise RuntimeError(f"unsupported platform: {system}")

libplt = library(
    name="plt",
    srcs=[*common_sources, backend_source],
    public_cflags=["-I$(S)", "-I$(S)/.."],
    cxxflags=locals().get("backend_cxxflags", []),
    deps=[libstd, *backend_deps],
    output="$(B)/libplt.a",
)

plt_unit_tests = program(
    name="plt_unit_tests",
    output="$(B)/plt_unit_tests",
    srcs=[
        "$(S)/main_ut.cpp",
        "$(S)/pointer_grab_ut.cpp",
    ],
    deps=[libplt, libstd],
)

plt_tests = command(
    name="plt_tests",
    outputs=["$(B)/plt_tests.stamp"],
    deps=[plt_unit_tests],
    cmd=[
        ["$(B)/plt_unit_tests"],
        [
            "python3", "-c",
            "from pathlib import Path; Path(r'$(B)/plt_tests.stamp').touch()",
        ],
    ],
    descr="TS",
    color="green",
)

install(libplt)
group("test", plt_tests)
