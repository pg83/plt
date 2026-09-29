"""Closing a preview inside direct key/text/flush callbacks preserves the editor."""
import os
from pathlib import Path
from session import Session

root = Path(os.environ["PLT_E2E_ARTIFACTS"])
for mode in ("key", "text", "flush"):
    os.environ["PLT_E2E_ARTIFACTS"] = str(root / mode)
    with Session(mode) as s:
        s.launch(PLT_DIRECT_INPUT="1", PLT_RETIRE=mode)
        preview = s.window("plt-repeat-preview")
        s.ipc(f'[con_id={preview["id"]}] move position 500 50')
        s.focus("plt-repeat-preview")
        s.screenshot("preview", [(.1, .5, .9, .8, "e0b040")], "plt-repeat-preview")
        s.ipc("input * repeat_delay 30")
        s.command("wtype", "-P", "a", "-s", "300", "-p", "a")
        s.logged("PREVIEW RETIRED")
        s.focus("plt-repeat-document")
        s.key("b")
        s.logged("TEXT 98")
        s.screenshot("survivor", [(.1, .5, .9, .8, "40a060")], "plt-repeat-document")
        s.close()
