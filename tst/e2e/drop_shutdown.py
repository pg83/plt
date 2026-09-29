"""Quit or replace a drag while the importer awaits a background validator."""
import os
from pathlib import Path
from session import Session

root = Path(os.environ["PLT_E2E_ARTIFACTS"])
for mode in ("pending-one", "pending-many", "pending-replace"):
    os.environ["PLT_E2E_ARTIFACTS"] = str(root / mode)
    with Session(mode) as s:
        s.launch(PLT_DROP_MODE=mode)
        s.logged("SOURCE READY")
        source = s.window("plt-drag-source")
        target = s.window("plt-drop-target")
        s.ipc(f'[con_id={source["id"]}] move position 20 50')
        s.ipc(f'[con_id={target["id"]}] move position 450 50')
        s.screenshot("pending-document", [(.1, .5, .9, .8, "204060")], "plt-drop-target")
        def drag():
            start = len((s.artifacts / "client.log").read_text())
            s.focus("plt-drag-source")
            s.pointer(100, 100, "plt-drag-source")
            s.button()
            s.wait(lambda: "DRAG STARTED" in (s.artifacts / "client.log").read_text()[start:], "new drag")
            s.pointer(100, 100, "plt-drop-target")
        drag()
        s.logged("VALIDATING 1")
        if mode != "pending-one":
            s.pointer(100, 100, "plt-drag-source")
            s.button(pressed=False)
            s.logged("SOURCE CANCELLED")
            drag()
            if mode == "pending-many":
                s.logged("VALIDATING 2")
            else:
                s.logged("HOVER 100")
                s.button(pressed=False)
                s.logged("DROPPED [dragged document]")
                s.screenshot("replacement-import", [(.1, .5, .9, .8, "40a060")], "plt-drop-target")
        s.ipc(f'[con_id={target["id"]}] kill')
        assert s.client.wait(timeout=10) == 0
        s.client = None
        s.input("button 272 0", client=False)
