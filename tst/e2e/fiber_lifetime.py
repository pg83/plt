"""API lifetime violations must stop at the invariant, before freed stacks are used."""
import os
from pathlib import Path
import signal
from session import Session

root = Path(os.environ["PLT_E2E_ARTIFACTS"])
for mode, invariant in (("park", "runtime != nullptr"), ("timeout", "runtime != nullptr"),
                        ("release", "runtime != scheduler.current()"), ("runtime", "scheduler.active != this")):
    os.environ["PLT_E2E_ARTIFACTS"] = str(root / mode)
    with Session(mode) as s:
        s.launch(PLT_LIFETIME=mode)
        s.focus()
        s.screenshot("ready", [(.1, .5, .9, .8, "204060")])
        s.key("a")
        assert s.client.wait(timeout=10) == -signal.SIGABRT
        log = (s.artifacts / "client.log").read_text()
        assert "INVALID LIFETIME REQUEST" in log and invariant + " failed" in log, log
        assert "ERROR: AddressSanitizer" not in log and "runtime error:" not in log, log
