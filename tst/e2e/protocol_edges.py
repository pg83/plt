"""A desktop remains usable across optional and malformed compositor events."""
import os
from pathlib import Path
from session import Session
from wire_proxy import Proxy

root = Path(os.environ["PLT_E2E_ARTIFACTS"])
cases = {
    "optional-events": [
        {"interface": "xdg_toplevel", "event": "configure", "append_state": 3},
        {"interface": "xdg_toplevel", "event": "configure", "insert": {
            "event": "configure_bounds", "values": {"width": 1024, "height": 768}}},
        {"interface": "wl_output", "event": "mode", "insert": {
            "event": "mode", "values": {"flags": 0, "width": 640, "height": 480, "refresh": 60000}}},
    ],
    "invalid-output": [
        {"interface": "wl_output", "event": "mode", "replace": {"width": 0, "height": 0}},
        {"interface": "wp_fractional_scale_v1", "event": "preferred_scale", "replace": {"scale": 0}},
    ],
    "held-at-focus": [{"interface": "wl_keyboard", "event": "enter", "replace": {"keys": [42, 54]}, "repeat": True}],
    "zero-pointer-serial": [{"interface": "wl_pointer", "event": "enter", "replace": {"serial": 0}}],
    "no-keymap": [{"interface": "wl_keyboard", "event": "keymap", "replace": {"format": 0}, "repeat": True}],
    "empty-keymap": [{"interface": "wl_keyboard", "event": "keymap", "replace": {"size": 0}, "repeat": True}],
}
for name, rules in cases.items():
    os.environ["PLT_E2E_ARTIFACTS"] = str(root / name)
    with Session(name) as s, Proxy(s, rules) as proxy:
        s.launch(WAYLAND_DISPLAY=proxy.path)
        s.focus()
        s.key("a")
        if name.endswith("keymap"):
            s.screenshot("ignored", [(.1, .5, .9, .8, "204060")])
            assert "TEXT 97\n" not in (s.artifacts / "client.log").read_text()
            for rule in proxy.rules:
                rule["repeat"] = False
        s.pointer(120, 100)
        s.key("b")
        s.logged("TEXT 98")
        s.screenshot("recovered", [(.1, .5, .9, .8, "40a060")])
        s.close()
