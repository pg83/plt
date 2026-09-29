"""Negotiate copy, move, refusal and abandoned transfers through real Wayland DnD."""
import os
from pathlib import Path
from session import Session

root = Path(os.environ["PLT_E2E_ARTIFACTS"])
for mode in ("copy", "move", "reject", "unknown", "none", "ignore", "partial", "leave", "pipe-failure", "flush-failure", "no-offer", "close-hover", "legacy-copy", "legacy-move", "renegotiate", "no-target"):
    os.environ["PLT_E2E_ARTIFACTS"] = str(root / mode)
    with Session(mode) as s:
        fault = "drop-flush@0" if mode == "flush-failure" else "selection-pipe@0" if mode == "pipe-failure" else "legacy-data-device@0" if mode.startswith("legacy-") else ""
        s.launch(PLT_DROP_MODE=mode.removeprefix("legacy-"), PLT_CHAOS=fault)
        s.logged("SOURCE READY")
        source = s.window("plt-drag-source")
        target = s.window("plt-drop-target")
        s.ipc(f'[con_id={source["id"]}] move position 20 50')
        s.ipc(f'[con_id={target["id"]}] move position 450 50')
        s.focus("plt-drag-source")
        s.screenshot("source", [(.1, .5, .9, .8, "e0b040")], app_id="plt-drag-source")
        s.pointer(100, 100, "plt-drag-source")
        s.button()
        s.logged("DRAG STARTED")
        s.pointer(100, 100, "plt-drop-target")
        if mode not in ("no-offer", "no-target"):
            s.logged("HOVER")
        if mode == "renegotiate":
            for x in (200, 300, 100):
                s.pointer(x, 100, "plt-drop-target")
                s.logged(f"HOVER {x}")
            s.logged("ACTION 2")
        if mode == "leave":
            s.pointer(100, 100, "plt-drag-source")
            s.logged("LEFT")
        if mode == "flush-failure":
            s.input("button 272 0", client=False)
            assert s.client.wait(timeout=10) == 0
            log = (s.artifacts / "client.log").read_text()
            assert "CHAOS drop-flush" in log and "DROPPED []" in log, log
            s.client = None
            continue
        s.button(pressed=False)
        if mode == "close-hover":
            s.logged("TARGET CLOSED")
            s.screenshot("surviving-source", [(.1, .5, .9, .8, "e0b040")], app_id="plt-drag-source")
            s.ipc(f'[con_id={source["id"]}] kill')
            assert s.client.wait(timeout=10) == 0
            s.client = None
            continue
        if mode == "no-offer":
            assert "DROPPED" not in (s.artifacts / "client.log").read_text()
        elif mode in ("legacy-copy", "legacy-move"):
            # Before wl_data_device v3 there is no action negotiation: copy.
            s.logged("DROPPED [dragged document]")
            s.logged("ACTION 1")
        elif mode in ("copy", "move", "renegotiate"):
            s.logged("DROPPED [dragged document]")
            s.logged("SOURCE FINISHED")
            s.logged("ACTION " + ("2" if mode == "move" else "1"))
        elif mode == "pipe-failure":
            s.logged("CHAOS selection-pipe")
            s.logged("DROPPED []")
        elif mode == "partial":
            s.logged("PARTIAL")
        elif mode == "ignore":
            s.logged("IGNORED")
        else:
            s.logged("SOURCE CANCELLED")
            assert "DROPPED" not in (s.artifacts / "client.log").read_text()
        color = "40a060" if mode in ("copy", "move", "partial", "pipe-failure", "legacy-copy", "legacy-move", "renegotiate") else "204060"
        s.screenshot("target", [(.1, .5, .9, .8, color)], app_id="plt-drop-target")
        s.ipc(f'[con_id={target["id"]}] kill')
        assert s.client.wait(timeout=10) == 0
        s.client = None
