"""Native resource allocation failures release ownership and permit a new desktop."""
import os
from pathlib import Path
import subprocess
import sys

root = Path(os.environ["PLT_E2E_ARTIFACTS"])
for fault in ("cocoa-mach-port", "cocoa-timer", "cocoa-descriptor", "cocoa-wake-source", "cocoa-descriptor-source"):
    out = root / fault
    out.mkdir()
    subprocess.run([sys.executable, str(Path(__file__).with_name("desktop.py"))],
                   env={**os.environ, "PLT_CHAOS": fault + "@0", "PLT_RECOVERY_ONLY": "1",
                        "PLT_E2E_ARTIFACTS": str(out)}, check=True, timeout=60)
    assert "RESOURCE FAILURE " in (out / "client.log").read_text()
