"""Negotiate copy, move, refusal and abandoned transfers through real Wayland DnD."""
import os
from pathlib import Path
from session import Session

root = Path(os.environ["PLT_E2E_ARTIFACTS"])
for mode in ("copy", "move", "reject", "unknown", "none", "ignore", "partial", "leave", "pipe-failure"):
    os.environ["PLT_E2E_ARTIFACTS"] = str(root / mode)
    with Session(mode) as s:
        s.launch(PLT_DROP_MODE=mode, PLT_CHAOS="selection-pipe@0" if mode == "pipe-failure" else "")
        source = s.window("plt-drag-source")
        target = s.window("plt-drop-target")
        s.ipc(f'[con_id={source["id"]}] move position 20 50')
        s.ipc(f'[con_id={target["id"]}] move position 450 50')
        s.pointer(100, 100, "plt-drag-source")
        s.button()
        s.logged("DRAG STARTED")
        s.pointer(100, 100, "plt-drop-target")
        s.logged("HOVER")
        if mode == "leave":
            s.pointer(100, 100, "plt-drag-source")
            s.logged("LEFT")
        s.button(pressed=False)
        if mode in ("copy", "move"):
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
        color = "40a060" if mode in ("copy", "move", "partial", "pipe-failure") else "204060"
        s.screenshot("target", [(.1, .5, .9, .8, color)], app_id="plt-drop-target")
        s.ipc(f'[con_id={target["id"]}] kill')
        assert s.client.wait(timeout=10) == 0
        s.client = None
