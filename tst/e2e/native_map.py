"""Resize a plt renderer whose embedding toolkit maps the native surface."""
import os
from pathlib import Path
import runpy

os.environ["PLT_NATIVE_MAP"] = "1"
runpy.run_path(str(Path(__file__).with_name("gallery.py")), run_name="__main__")
