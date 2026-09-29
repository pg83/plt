"""Real clipboard peers survive a failed transfer and retry the same operation."""
import os
from pathlib import Path
import subprocess
from session import Session

root = Path(os.environ["PLT_E2E_ARTIFACTS"])
for fault in ("selection-pipe", "selection-flags", "selection-set-flags", "selection-read", "read-interrupted", "read-again",
              "selection-write", "write-interrupted", "write-again", "write-zero", "compose-table",
              "keymap-map", "keymap-compile", "keymap-state", "send-flags", "send-set-flags", "utf8-string"):
    os.environ["PLT_E2E_ARTIFACTS"] = str(root / fault)
    with Session(fault) as s:
        injected = {"send-flags": "selection-flags", "send-set-flags": "selection-set-flags"}.get(fault, fault)
        s.launch(PLT_CHAOS="" if fault == "utf8-string" else f"{injected}@0")
        s.focus()
        if fault in ("selection-write", "write-interrupted", "write-again", "write-zero", "send-flags", "send-set-flags"):
            s.key("c")
            s.logged("COPY")
            value = s.command("wl-paste", "--no-newline")
            assert value == (b"" if fault in ("selection-write", "send-flags", "send-set-flags") else b"recovered"), value
            s.logged(f"CHAOS {injected}")
            assert s.command("wl-paste", "--no-newline") == b"recovered"
        else:
            payload = Path(s.runtime.name) / "clipboard.txt"
            payload.write_text("recovered")
            with payload.open("rb") as data:
                copy = subprocess.Popen(["wl-copy", "--foreground", "--type", "UTF8_STRING" if fault == "utf8-string" else "text/plain;charset=utf-8"], stdin=data, env=s.env)
            s.processes.append(copy)
            def clipboard_ready():
                result = subprocess.run(["wl-paste", "--no-newline"], env=s.env,
                                        capture_output=True, timeout=5)
                return result.returncode == 0 and result.stdout == b"recovered"
            s.wait(clipboard_ready, "clipboard owner")
            s.key("v")
            if fault != "utf8-string":
                s.logged(f"CHAOS {injected}")
            failed = fault in ("selection-pipe", "selection-flags", "selection-set-flags", "selection-read")
            s.logged("PASTE []" if failed else "PASTE [recovered]")
            if failed:
                s.screenshot("failed", [(.1, .5, .9, .8, "c04040")])
                s.key("v")
                s.logged("PASTE [recovered]")
            s.screenshot("recovered", [(.1, .5, .9, .8, "40a060")])
        s.close()
