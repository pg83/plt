"""Fail native startup stages, release resources and reconnect to the same compositor."""
import os
from pathlib import Path
from session import Session

root = Path(os.environ["PLT_E2E_ARTIFACTS"])
for rule in ("display-connect@0", "xkb-context@0", "registry-roundtrip@0", "registry-roundtrip@1",
             "no-compositor@0", "no-shell@0", "no-seat@0"):
    os.environ["PLT_E2E_ARTIFACTS"] = str(root / rule.replace("@", "-"))
    with Session(rule) as s:
        failed = s.start([str(s.binary)], "failed", PLT_CHAOS=rule)
        assert failed.wait(timeout=10) == 2
        log = (s.artifacts / "failed.log").read_text()
        assert "STARTUP ERROR " in log and "CHAOS " + rule.split("@")[0] in log, log
        s.launch(PLT_CHAOS="", LC_ALL="", LC_CTYPE="", LANG="")
        s.screenshot("reconnected", [(.1, .5, .9, .8, "40a060")])
        s.close()
