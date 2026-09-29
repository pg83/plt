"""A renderer's private queue reads events while an application fd is dispatched."""
import os
from pathlib import Path
from session import Session

root = Path(os.environ["PLT_E2E_ARTIFACTS"])
for fault in ("", "pending-dispatch@0"):
    os.environ["PLT_E2E_ARTIFACTS"] = str(root / ("failure" if fault else "success"))
    with Session("backlog") as s:
        fifo = Path(s.runtime.name) / "commands"
        os.mkfifo(fifo)
        s.launch(PLT_COMMAND_PIPE=str(fifo), PLT_CHAOS=fault)
        s.focus()
        s.screenshot("connected", [(.1, .5, .9, .8, "40a060")])
        with fifo.open("wb", buffering=0) as commands:
            s.key("p")
            s.logged("DISPATCH PAUSED")
            s.key("a")
            commands.write(b"r!")
            if fault:
                assert s.client.wait(timeout=10) == 0
                log = (s.artifacts / "client.log").read_text()
                assert "FOREIGN QUEUE DONE" in log and "CHAOS pending-dispatch" in log, log
                s.client = None
            else:
                s.logged("BACKLOG DISPATCHED")
                s.screenshot("dispatched", [(.1, .5, .9, .8, "40a060")])
                s.close()
