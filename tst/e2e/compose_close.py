"""An IME commit closes one document; its remaining scalars must not reach freed input."""
import os
from session import Session

os.environ["PLT_IME"] = "1"
with Session("compose-close") as s:
    s.launch(PLT_DIRECT_INPUT="1", PLT_CLOSE_ON_TEXT="1")
    survivor = s.window("plt-compose-survivor")
    s.ipc(f'[con_id={survivor["id"]}] move position 500 50')
    s.focus("plt-compose-editor")
    s.input("ime-preedit 6162")
    s.logged("PREEDIT 2 0 2")
    s.screenshot("composing", [(.1, .5, .9, .8, "e0b040")], "plt-compose-editor")
    s.input("ime-commit 414243")
    s.logged("EDITOR CLOSED")
    assert "TEXT 1 65" in (s.artifacts / "client.log").read_text()
    assert "TEXT 2 " not in (s.artifacts / "client.log").read_text()
    s.focus("plt-compose-survivor")
    s.key("b")
    s.logged("TEXT 1 98")
    s.screenshot("survivor", [(.1, .5, .9, .8, "40a060")], "plt-compose-survivor")
    s.close()
