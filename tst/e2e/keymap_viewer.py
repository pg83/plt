"""Shortcut inspection with uncommon hardware keysyms, compose and held keys."""
import re
from session import Session

with Session("keymap-viewer") as s:
    s.launch(LC_ALL="C.UTF-8")
    s.focus()
    keys = {"Clear": 17, "ISO_Left_Tab": 6, "Menu": 93,
            "KP_Separator": 70, "KP_F1": 71, "KP_F2": 72, "KP_F3": 73,
            "KP_F4": 74, "KP_Space": 86, "KP_Tab": 87}
    for name, identity in keys.items():
        s.key(name)
        s.logged(f"KEY {identity} 0 ")
    for modifier, flag in (("alt", 4), ("logo", 8)):
        s.key("a", modifier)
        s.logged(f"KEY 1 0 {flag} 97")
    s.command("wtype", "-k", "dead_acute", "-k", "e")
    s.logged("TEXT 233")
    s.command("wtype", "-k", "dead_acute", "-k", "F1")
    s.key("a")
    s.logged("TEXT 97")
    s.ipc("input * repeat_delay 30")
    s.command("wtype", "-P", "a", "-s", "200", "-p", "a")
    s.wait(lambda: re.search(r"KEY 1 2 ", (s.artifacts / "client.log").read_text()), "held key repeats")
    s.screenshot("keys", [(.1, .5, .9, .8, "40a060")])
    s.close()
