"""Stale IME focus notifications cannot commit text into the wrong document."""
import os
from session import Session
from wire_proxy import Proxy

os.environ["PLT_IME"] = "1"
with Session("compose-focus") as s, Proxy(s, []) as proxy:
    s.launch(WAYLAND_DISPLAY=proxy.path, PLT_EXTRA_PASSIVE="1")
    preview = s.window("plt-compose-survivor")
    s.ipc(f'[con_id={preview["id"]}] move position 500 50')
    s.focus("plt-compose-editor")
    s.input("ime-preedit 61")
    s.logged("PREEDIT 1 0 1")
    s.screenshot("composing", [(.1, .5, .9, .8, "e0b040")], "plt-compose-editor")
    active = proxy.surface("plt-compose-editor")
    passive = proxy.surface("plt-compose-survivor")
    interface = "zwp_text_input_v3"
    proxy.event(interface, "enter", surface=passive)
    proxy.event(interface, "leave", surface=passive)
    proxy.event(interface, "enter", surface=passive)
    proxy.event(interface, "commit_string", text="C")
    proxy.event(interface, "done", serial=0)
    proxy.event(interface, "enter", surface=active)
    proxy.event(interface, "leave", surface=passive)
    s.input("ime-commit 42")
    s.logged("TEXT 1 66")
    assert "TEXT 1 67" not in (s.artifacts / "client.log").read_text()
    s.screenshot("editor", [(.1, .5, .9, .8, "40a060")], "plt-compose-editor")
    s.screenshot("passive", [(.1, .5, .9, .8, "204060")], "plt-compose-survivor")
    s.close()
