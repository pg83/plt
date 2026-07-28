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
elif build.target != build.host:
    raise RuntimeError(f"unsupported target: {target_platform}")
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
    server_protocol_outputs = []
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
        if protocol in {
            "xdg-shell",
            "viewporter",
            "fractional-scale-v1",
            "xdg-decoration-unstable-v1",
            "xdg-activation-v1",
            "primary-selection-unstable-v1",
            "cursor-shape-v1",
        }:
            server_header = f"$(B)/protocol/{protocol}-server-protocol.h"
            server_protocol_outputs.append(server_header)
            protocol_outputs.append(server_header)
            protocol_commands.append(
                ["wayland-scanner", "server-header", source, server_header],
            )
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
        dependency(ldflags=["-lrt", "-lpthread"]),
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

if build.target == build.host:
    plt_unit_tests = program(
        name="plt_unit_tests",
        output="$(B)/plt_unit_tests",
        srcs=[
            "$(S)/test_ut.cpp",
            "$(S)/pointer_grab_ut.cpp",
        ],
        deps=[libplt, libstd],
    )

    test_deps = [plt_unit_tests]
    test_commands = [["$(B)/plt_unit_tests"]]
    if system == "Linux":
        plt_wayland_integration_tests = program(
            name="plt_wayland_integration_tests",
            output="$(B)/plt_wayland_integration_tests",
            srcs=[{
                "src": "$(S)/test_wayland.cpp",
                "inputs": server_protocol_outputs,
            }],
            deps=[
                libplt,
                libstd,
                pkg_config("wayland-server >= 1.20"),
                pkg_config("xkbcommon >= 1.0"),
            ],
        )
        test_deps.append(plt_wayland_integration_tests)
        test_commands.append(["$(B)/plt_wayland_integration_tests"])

    plt_tests = command(
        name="plt_tests",
        outputs=["$(B)/plt_tests.stamp"],
        deps=test_deps,
        cmd=[
            *test_commands,
            [
                "python3", "-c",
                "from pathlib import Path; Path(r'$(B)/plt_tests.stamp').touch()",
            ],
        ],
        descr="TS",
        color="green",
    )
    group("test", plt_tests)

install(libplt)
