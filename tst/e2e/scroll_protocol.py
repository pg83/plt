"""Touchpad continuation, wheel termination, and unknown axes through Sway."""
import os
from pathlib import Path
from session import Session
from wire_proxy import Proxy

root = Path(os.environ["PLT_E2E_ARTIFACTS"])
cases = {
    "continuation": [{"interface": "wl_pointer", "event": "axis_source", "skip": 1, "drop": True}],
    "wheel-stop": [{"interface": "wl_pointer", "event": "axis", "insert": {
        "event": "axis_stop", "values": {"time": 0, "axis": 0}}}],
    "unknown-axis": [
        {"interface": "wl_pointer", "event": "axis", "replace": {"axis": 42}},
        {"interface": "wl_pointer", "event": "axis_value120", "replace": {"axis": 42}},
    ],
}
for name, rules in cases.items():
    os.environ["PLT_E2E_ARTIFACTS"] = str(root / name)
    with Session(name) as s, Proxy(s, rules) as proxy:
        s.launch(WAYLAND_DISPLAY=proxy.path)
        s.pointer(160, 130)
        if name == "continuation":
            s.input("smooth 0 12")
            s.logged(" 1 1 ")
            s.input("smooth 0 8")
            s.logged(" 2 1 ")
            s.input("stop 0")
            s.logged(" 3 1 ")
        else:
            s.input("wheel 0 1")
            if name == "unknown-axis":
                s.screenshot("ignored", [(.1, .5, .9, .8, "204060")])
            s.input("wheel 0 1")
        s.screenshot("scrolled", [(.1, .5, .9, .8, "40a060")])
        s.close()
