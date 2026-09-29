"""Retry a temporarily unwritable display before opening the first document."""
from session import Session

with Session("connection-warmup") as s:
    s.launch(PLT_CHAOS="flush-again@1")
    s.logged("CHAOS flush-again")
    s.logged("CONNECTION WARMED UP")
    s.screenshot("document", [(.1, .5, .9, .8, "204060")])
    s.focus()
    s.close()
