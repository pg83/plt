"""Real clipboard peers survive a failed transfer and retry the same operation."""
import os
from pathlib import Path
import subprocess
from session import Session

root = Path(os.environ["PLT_E2E_ARTIFACTS"])
for fault in ("selection-pipe", "selection-flags", "selection-read", "read-interrupted",
              "selection-write", "write-interrupted", "compose-table",
              "keymap-map", "keymap-compile", "keymap-state"):
    os.environ["PLT_E2E_ARTIFACTS"] = str(root / fault)
    with Session(fault) as s:
        s.launch(PLT_CHAOS=f"{fault}@0")
        s.focus()
        if fault in ("selection-write", "write-interrupted"):
            s.key("c")
            s.logged("COPY")
            value = s.command("wl-paste", "--no-newline")
            assert value == (b"" if fault == "selection-write" else b"recovered"), value
            s.logged(f"CHAOS {fault}")
            assert s.command("wl-paste", "--no-newline") == b"recovered"
        else:
            payload = Path(s.runtime.name) / "clipboard.txt"
            payload.write_text("recovered")
            with payload.open("rb") as data:
                copy = subprocess.Popen(["wl-copy", "--foreground", "--type", "text/plain;charset=utf-8"], stdin=data, env=s.env)
            s.processes.append(copy)
            s.wait(lambda: s.command("wl-paste", "--no-newline") == b"recovered", "clipboard owner")
            s.key("v")
            s.logged(f"CHAOS {fault}")
            failed = fault in ("selection-pipe", "selection-flags", "selection-read")
            s.logged("PASTE []" if failed else "PASTE [recovered]")
            if failed:
                s.screenshot("failed", [(.1, .5, .9, .8, "c04040")])
                s.key("v")
                s.logged("PASTE [recovered]")
            s.screenshot("recovered", [(.1, .5, .9, .8, "40a060")])
        s.close()
