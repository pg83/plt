"""Real desktop previews on compositors with optional protocols unavailable."""
import os
from pathlib import Path
from session import Session

root = Path(os.environ["PLT_E2E_ARTIFACTS"])
cases = ["no-clipboard", "no-primary", "no-text-input", "no-viewport", "no-fractional-scale",
         "no-decoration", "no-activation", "no-cursor-shape", "no-output", "legacy-seat", "flush-interrupted", "flush-again", "poll-interrupted", "passive"]
for case in cases:
    os.environ["PLT_E2E_ARTIFACTS"] = str(root / case)
    with Session(case) as s:
        fifo = Path(s.runtime.name) / "commands"
        os.mkfifo(fifo)
        s.launch(PLT_COMMAND_PIPE=str(fifo), PLT_CHAOS="" if case == "passive" else case + "@0",
                 PLT_PASSIVE="1" if case == "passive" else "0")
        with fifo.open("w", buffering=1) as commands:
            commands.write("c\n")
            s.logged("COMMAND c")
            s.focus()
            s.pointer(200, 140)
            s.key("a")
            s.click(200, 140)
            s.input("scroll 2")
            commands.write("w\n")
            s.logged("COMMAND w")
            if case != "passive":
                s.logged("CHAOS " + case)
                s.logged("KEY 97 ")
            else:
                assert "KEY " not in (s.artifacts / "client.log").read_text()
            before = (s.artifacts / "client.log").read_text().count("COMMAND c")
            commands.write("c\n")
            s.wait(lambda: (s.artifacts / "client.log").read_text().count("COMMAND c") > before, "replace selection")
            s.screenshot("preview", [(.1, .5, .9, .8, "40a060")])
            commands.write("q\n")
            assert s.client.wait(timeout=10) == 0
            s.client = None
