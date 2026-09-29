"""A clipboard consumer exits during a large document transfer; copying still works."""
import os
from pathlib import Path
import subprocess
from session import Session

root = Path(os.environ["PLT_E2E_ARTIFACTS"])
for mode in ("interrupted", "ignored", "pending"):
    os.environ["PLT_E2E_ARTIFACTS"] = str(root / mode)
    with Session("clipboard-abort") as s:
        s.launch(PLT_LARGE_COPY="1", PLT_CHAOS="signal-wait-interrupted@0" if mode == "interrupted" else "",
                 PLT_IGNORE_SIGPIPE="1" if mode == "ignored" else "0", PLT_PENDING_SIGPIPE="1" if mode == "pending" else "0")
        s.focus()
        s.key("c")
        s.logged("COPY")
        reader = subprocess.Popen(["wl-paste", "--no-newline"], env=s.env, stdout=subprocess.PIPE)
        s.processes.append(reader)
        assert reader.stdout.read(1) == b"x"
        reader.terminate()
        reader.wait(timeout=5)
        reader.stdout.close()
        if mode == "interrupted":
            s.logged("CHAOS signal-wait-interrupted")
        # A different consumer receives the entire same selection after abandonment.
        assert s.command("wl-paste", "--no-newline") == b"x" * (4096 * 1024)
        s.screenshot("available-after-abandon", [(.1, .5, .9, .8, "204060")])
        s.close()
        if mode == "pending":
            assert "PENDING SIGPIPE PRESERVED" in (s.artifacts / "client.log").read_text()
