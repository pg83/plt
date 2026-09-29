"""Malformed drag sequences cannot invoke an absent handler or import an absent offer."""
import os
from pathlib import Path
from session import Session
from wire_proxy import Proxy

root = Path(os.environ["PLT_E2E_ARTIFACTS"])
for mode in ("missing-offer", "duplicate-enter", "no-target"):
    rules = [{"interface": "wl_data_device", "event": "leave", "first_object": True, "after_surface": "plt-drop-target",
              "insert": {"event": "drop", "values": {}}}]
    if mode != "no-target":
        rules.insert(0, {"interface": "wl_data_device", "event": "enter", "first_object": True,
                         "surface_app": "plt-drop-target",
                         "replace" if mode == "missing-offer" else "duplicate": {"id": 0}})
    os.environ["PLT_E2E_ARTIFACTS"] = str(root / mode)
    with Session(mode) as s, Proxy(s, rules) as proxy:
        s.launch(WAYLAND_DISPLAY=proxy.path, PLT_DROP_MODE="no-target" if mode == "no-target" else "reject")
        s.logged("SOURCE READY")
        source = s.window("plt-drag-source")
        target = s.window("plt-drop-target")
        s.ipc(f'[con_id={source["id"]}] move position 20 50')
        s.ipc(f'[con_id={target["id"]}] move position 450 50')
        s.screenshot("source", [(.1, .5, .9, .8, "e0b040")], "plt-drag-source")
        s.pointer(100, 100, "plt-drag-source")
        s.button()
        s.logged("DRAG STARTED")
        s.pointer(100, 100, "plt-drop-target")
        s.screenshot("rejected", [(.1, .5, .9, .8, "204060")], "plt-drop-target")
        s.button(pressed=False)
        s.logged("SOURCE CANCELLED")
        assert "DROPPED" not in (s.artifacts / "client.log").read_text()
        s.ipc(f'[con_id={target["id"]}] kill')
        assert s.client.wait(timeout=10) == 0
        s.client = None
