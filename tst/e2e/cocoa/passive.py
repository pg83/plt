"""Passive native previews ignore input while continuing to render."""
import os
from pathlib import Path
import subprocess
import sys

subprocess.run([sys.executable, str(Path(__file__).with_name("desktop.py"))],
               env={**os.environ, "PLT_PASSIVE": "1"}, check=True, timeout=45)
