"""Native drag rejection and encoding allocation failures preserve the desktop."""
import os
from pathlib import Path
import subprocess
import sys

root = Path(os.environ["PLT_E2E_ARTIFACTS"])
for mode in ("no-target", "encode-failure"):
    out = root / mode
    out.mkdir()
    env = {"PLT_NO_DROP": "1"} if mode == "no-target" else {"PLT_DROP_FAILURE": "1", "PLT_CHAOS": "cocoa-drop-encode@0"}
    subprocess.run([sys.executable, str(Path(__file__).with_name("desktop.py"))],
                   env={**os.environ, **env, "PLT_E2E_ARTIFACTS": str(out)}, check=True, timeout=45)
