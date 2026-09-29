"""A severed compositor socket ends the desktop cleanly and permits reconnect."""
from session import Session
from wire_proxy import Proxy

with Session("connection-loss") as s:
    with Proxy(s, []) as proxy:
        s.launch(WAYLAND_DISPLAY=proxy.path)
        s.screenshot("connected", [(.1, .5, .9, .8, "40a060")])
        proxy.disconnect()
        assert s.client.wait(timeout=10) == 0
        assert "CLOSED plt-startup" in (s.artifacts / "client.log").read_text()
        s.client = None
    s.wait(lambda: not s.windows(), "retired connection")
    s.launch()
    s.screenshot("reconnected", [(.1, .5, .9, .8, "40a060")])
    s.close()
