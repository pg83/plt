"""Retire a renderer's window while keyboard, pointer and IME events await dispatch."""
import os
from pathlib import Path
from session import Session

os.environ["PLT_IME"] = "1"
with Session("window-lifetime") as s:
    fifo = Path(s.runtime.name) / "commands"
    os.mkfifo(fifo)
    s.launch(PLT_COMMAND_PIPE=str(fifo))
    desktop = s.window("plt-lifetime-desktop")
    preview = s.window("plt-lifetime-preview")
    s.ipc(f'[con_id={preview["id"]}] move position 500 50')
    s.focus("plt-lifetime-desktop")
    s.screenshot("preview", [(.1, .5, .9, .8, "e0b040")], "plt-lifetime-preview")
    with fifo.open("wb", buffering=0) as commands:
        s.key("p")
        s.logged("DISPATCH PAUSED")
        s.focus("plt-lifetime-preview")
        s.pointer(100, 100, "plt-lifetime-preview")
        s.button()
        s.input("wheel 0 1")
        s.input("smooth 1 5")
        s.input("stop 1")
        s.key("a")
        s.input("ime-commit 41")
        s.button(pressed=False)
        s.pointer(110, 100, "plt-lifetime-preview")
        commands.write(b"r!")
        s.logged("PREVIEW RETIRED")
    s.focus("plt-lifetime-desktop")
    s.screenshot("surviving-desktop", [(.1, .5, .9, .8, "40a060")], "plt-lifetime-desktop")
    s.close()
