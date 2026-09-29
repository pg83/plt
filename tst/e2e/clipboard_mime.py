"""Exchange selections with a peer offering exactly one supported or unknown MIME."""
import os
from pathlib import Path
from session import Session

root = Path(os.environ["PLT_E2E_ARTIFACTS"])
for mime, expected in (("UTF8_STRING", "dragged document"), ("text/plain", "dragged document"),
                       ("application/octet-stream", "")):
    os.environ["PLT_E2E_ARTIFACTS"] = str(root / mime.replace("/", "-"))
    with Session(mime) as s:
        s.launch(PLT_SELECTION_MIME=mime)
        s.logged("SOURCE READY")
        source = s.window("plt-drag-source")
        target = s.window("plt-drop-target")
        s.ipc(f'[con_id={source["id"]}] move position 20 50')
        s.ipc(f'[con_id={target["id"]}] move position 450 50')
        s.focus("plt-drag-source")
        s.click(100, 100, app_id="plt-drag-source")
        s.logged("SELECTION READY")
        s.focus("plt-drop-target")
        s.key("p")
        s.logged(f"PASTED [{expected}]")
        if expected:
            s.logged("SENT " + mime)
        s.screenshot("pasted", [(.1, .5, .9, .8, "40a060")], app_id="plt-drop-target")
        s.close()
