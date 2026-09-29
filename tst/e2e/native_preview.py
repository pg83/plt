"""A plt editor embeds a native toolkit preview on the same Wayland display."""
import os
from session import Session

os.environ["PLT_IME"] = "1"
with Session("native-preview") as s:
    s.launch()
    preview = s.window("plt-native-preview")
    s.ipc(f'[con_id={preview["id"]}] move position 500 50')
    s.focus("plt-native-preview")
    s.pointer(100, 100, "plt-native-preview")
    s.key("a")
    s.screenshot("native", [(.1, .5, .9, .8, "e0b040")], "plt-native-preview")
    assert "TEXT 97" not in (s.artifacts / "client.log").read_text()
    s.focus("plt-native-editor")
    s.key("b")
    s.logged("TEXT 98")
    s.screenshot("editor", [(.1, .5, .9, .8, "40a060")], "plt-native-editor")
    s.close()
