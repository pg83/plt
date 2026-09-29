"""Close a terminal while its input fiber is blocked and more input is queued."""
from session import Session

with Session("input-cancel") as s:
    s.launch()
    s.focus()
    s.type("a")
    s.logged("INPUT BLOCKED")
    s.type("queued input")
    s.pointer(100, 100)
    s.button()
    s.button(pressed=False)
    s.screenshot("waiting", [(.1, .5, .9, .8, "e0b040")])
    node = s.window()
    s.ipc(f'[con_id={node["id"]}] kill')
    assert s.client.wait(timeout=10) == 0
    assert "INPUT QUEUE RELEASED" in (s.artifacts / "client.log").read_text()
