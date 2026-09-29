"""WindowServer rejects a drag into a native window with no drop handler."""
import os
from pathlib import Path
import subprocess
import sys

subprocess.run([sys.executable, str(Path(__file__).with_name("desktop.py"))],
               env={**os.environ, "PLT_NO_DROP": "1"}, check=True, timeout=75)
