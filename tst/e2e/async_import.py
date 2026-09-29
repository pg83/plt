"""Run the native importer on a real Wayland connection, including cancellations."""
import os
from pathlib import Path
import subprocess
import sys
from session import Session

with Session("async-import") as s:
    result = subprocess.run([sys.executable, str(Path(__file__).parent / "cocoa/importer.py")],
                            env=s.env, capture_output=True, timeout=30)
    assert result.returncode == 0, result.stdout.decode() + result.stderr.decode()
