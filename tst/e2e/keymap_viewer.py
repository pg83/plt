"""Shortcut inspection with uncommon hardware keysyms, compose and held keys."""
import os
from pathlib import Path
import re
from session import Session

root = Path(os.environ["PLT_E2E_ARTIFACTS"])
os.environ["PLT_E2E_ARTIFACTS"] = str(root / "standard")
with Session("keymap-viewer") as s:
    s.launch(LC_ALL="C.UTF-8")
    s.focus()
    keys = {"Clear": 17, "ISO_Left_Tab": 6, "Menu": 93,
            "KP_Separator": 70, "KP_F1": 71, "KP_F2": 72, "KP_F3": 73,
            "KP_F4": 74, "KP_Space": 86, "KP_Tab": 87}
    for name, identity in keys.items():
        s.key(name)
        s.logged(f"KEY {identity} 0 ")
    for modifier, flag in (("alt", 4), ("logo", 8), ("altgr", 64)):
        s.key("a", modifier)
        s.logged(f"KEY 1 0 {flag} 97")
    s.command("wtype", "-k", "dead_acute", "-k", "e")
    s.logged("TEXT 233")
    s.command("wtype", "-k", "dead_acute", "-k", "F1")
    s.key("a")
    s.logged("TEXT 97")
    s.ipc("input * repeat_delay 30")
    s.command("wtype", "-P", "a", "-s", "200", "-p", "a")
    s.wait(lambda: re.search(r"KEY 1 1 ", (s.artifacts / "client.log").read_text()), "held key repeats")
    s.command("wtype", "-P", "dead_acute", "-s", "150", "-p", "dead_acute", "-k", "e")
    s.command("wtype", "-P", "F1", "-s", "150", "-p", "F1")
    s.command("wtype", "-P", "a", "-P", "b", "-p", "a", "-p", "b")
    start = len((s.artifacts / "client.log").read_text())
    held = s.start(["wtype", "-P", "a", "-s", "1000", "-p", "a"], "repeat-disabled")
    s.wait(lambda: re.search(r"KEY 1 1 ", (s.artifacts / "client.log").read_text()[start:]), "repeat before disabling")
    s.ipc("input * repeat_rate 0")
    assert held.wait(timeout=5) == 0
    s.key("z")
    s.logged("TEXT 122")
    s.screenshot("keys", [(.1, .5, .9, .8, "40a060")])
    s.close()

# User compose rules can emit multiple characters or only a non-text keysym.
os.environ["PLT_E2E_ARTIFACTS"] = str(root / "custom-compose")
with Session("custom-compose") as s:
    compose = Path(s.runtime.name) / "Compose"
    compose.write_text('<Multi_key> <a> : "ABCDEFGHIJK"\n<Multi_key> <b> : F1\n')
    s.launch(XCOMPOSEFILE=str(compose), LC_ALL="C.UTF-8")
    s.focus()
    s.command("wtype", "-k", "Multi_key", "-k", "a")
    s.logged("TEXT 72")
    log = (s.artifacts / "client.log").read_text()
    assert all(f"TEXT {value}\n" in log for value in range(65, 73)), log
    assert "TEXT 73\n" not in log, log
    start = len(log)
    s.command("wtype", "-k", "Multi_key", "-k", "b")
    s.key("z")
    s.logged("TEXT 122")
    log = (s.artifacts / "client.log").read_text()[start:]
    assert "TEXT 98\n" not in log, log
    s.screenshot("composed", [(.1, .5, .9, .8, "40a060")])
    s.close()
