"""A renderer retires its preview with a real cross-process drag enter queued."""
import os
from pathlib import Path
import subprocess
from session import Session

with Session("drop-lifetime") as s:
    fifo = Path(s.runtime.name) / "commands"
    os.mkfifo(fifo)
    s.launch(PLT_COMMAND_PIPE=str(fifo))
    with (s.artifacts / "source.log").open("w") as log:
        source = subprocess.Popen([str(s.binary.with_name("drop_import"))],
            env={**s.env, "PLT_SOURCE_ONLY": "1"}, stdout=log, stderr=subprocess.STDOUT)
    try:
        s.wait(lambda: "SOURCE READY" in (s.artifacts / "source.log").read_text(), "external drag source")
        preview = s.window("plt-lifetime-preview")
        source_window = s.window("plt-drag-source")
        s.ipc(f'[con_id={source_window["id"]}] move position 20 400')
        s.ipc(f'[con_id={preview["id"]}] move position 500 50')
        s.screenshot("preview", [(.1, .5, .9, .8, "e0b040")], "plt-lifetime-preview")
        s.focus("plt-lifetime-desktop")
        with fifo.open("wb", buffering=0) as commands:
            s.key("p")
            s.logged("DISPATCH PAUSED")
            s.focus("plt-drag-source")
            s.pointer(100, 100, "plt-drag-source")
            s.button()
            s.wait(lambda: "DRAG STARTED" in (s.artifacts / "source.log").read_text(), "external drag started")
            s.pointer(100, 100, "plt-lifetime-preview")
            commands.write(b"r!")
            s.logged("PREVIEW RETIRED")
        s.button(pressed=False)
        s.focus("plt-lifetime-desktop")
        s.screenshot("surviving-desktop", [(.1, .5, .9, .8, "40a060")], "plt-lifetime-desktop")
        s.close()
        s.ipc(f'[con_id={source_window["id"]}] kill')
        assert source.wait(timeout=10) == 0
    finally:
        if source.poll() is None:
            source.terminate()
            source.wait(timeout=10)
