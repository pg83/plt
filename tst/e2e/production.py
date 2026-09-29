"""The installed archive renders and accepts input with test controls present."""
from session import Session

with Session("production") as s:
    s.launch(PLT_CHAOS="display-connect@0,wake-pipe@0")
    s.logged("READY TO IMPORT")
    s.screenshot("production", [(.1, .5, .9, .8, "40a060")])
    s.close()
    assert "CHAOS " not in (s.artifacts / "client.log").read_text()
