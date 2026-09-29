"""I/O failures stop the desktop loop cleanly; a new connection remains usable."""
import os
from pathlib import Path
import time
from session import Session

root = Path(os.environ["PLT_E2E_ARTIFACTS"])
for rule in ("loop-flush@0", "flush-error@0", "flush-error@3", "display-read@0", "display-dispatch@0",
             "display-dispatch@1", "display-dispatch@2", "display-dispatch@3"):
    os.environ["PLT_E2E_ARTIFACTS"] = str(root / rule.replace("@", "-"))
    with Session(rule) as s:
        failed = s.start([str(s.binary)], "failed", PLT_CHAOS=rule)
        deadline = time.monotonic() + 10
        while failed.poll() is None and time.monotonic() < deadline:
            if s.windows():
                s.key("a")
            time.sleep(.05)
        assert failed.wait(timeout=3) == 0
        log = (s.artifacts / "failed.log").read_text()
        assert "CHAOS " + rule.split("@")[0] in log and "CLOSED plt-startup" in log, log
        s.launch(PLT_CHAOS="")
        s.screenshot("reconnected", [(.1, .5, .9, .8, "40a060")])
        s.close()
