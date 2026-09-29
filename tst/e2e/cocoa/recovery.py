"""Render and accept input after unavailable display pacing or an interrupted poll."""
import os
from pathlib import Path
import subprocess
import sys

root = Path(os.environ["PLT_E2E_ARTIFACTS"])
for fault in ("display-link", "display-callback", "poll-interrupted", "cocoa-key-source", "cocoa-key-layout", "cocoa-key-translate", "no-app-asn", "no-app-label"):
    out = root / fault
    out.mkdir()
    subprocess.run([sys.executable, str(Path(__file__).with_name("desktop.py"))],
                   env={**os.environ, "PLT_CHAOS": fault + "@0", "PLT_RECOVERY_ONLY": "1",
                        "PLT_E2E_ARTIFACTS": str(out)}, check=True, timeout=60)
