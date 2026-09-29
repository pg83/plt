"""A live editor queues operations while its compositor seat is withdrawn."""
import os
from pathlib import Path
from session import Session
from wire_proxy import Proxy

root = Path(os.environ["PLT_E2E_ARTIFACTS"])
for fault in ("", "no-text-input", "no-clipboard", "no-primary", "no-cursor-shape"):
    os.environ["PLT_E2E_ARTIFACTS"] = str(root / (fault or "all-protocols"))
    with Session("seat-lifetime") as s, Proxy(s, []) as proxy:
        fifo = Path(s.runtime.name) / "commands"
        os.mkfifo(fifo)
        s.launch(WAYLAND_DISPLAY=proxy.path, PLT_COMMAND_PIPE=str(fifo), PLT_CHAOS=fault + "@0" if fault else "")
        s.focus()
        s.key("a")
        s.logged("KEY 97 ")
        with fifo.open("w", buffering=1) as commands:
            proxy.global_available("wl_seat", False)
            commands.write("c\nw\n")
            s.logged("COMMAND w")
            s.screenshot("seat-removed", [(.1, .5, .9, .8, "40a060")])
            proxy.global_available("wl_seat", True)
            s.key("b")
            s.logged("KEY 98 ")
            s.screenshot("seat-restored", [(.1, .5, .9, .8, "40a060")])
            if fault != "no-clipboard":
                assert s.command("wl-paste", "--no-newline") == b"preview selection"
            commands.write("q\n")
            assert s.client.wait(timeout=10) == 0
            s.client = None
