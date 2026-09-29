from session import Session

with Session("scroller") as s:
    s.launch()
    s.screenshot("before", [(.1, .5, .9, .8, "204060")])
    s.pointer(160, 130)
    s.button("button5")
    s.logged("SCROLL ")
    s.screenshot("scrolled", [(.1, .5, .9, .8, "40a060")])
    s.input("wheel 1 2")
    s.logged(" 0 0 2.000")
    for axis in (0, 1):
        s.input(f"smooth {axis} 12")
        s.input(f"smooth {axis} 8")
        s.input(f"stop {axis}")
    log = (s.artifacts / "client.log").read_text()
    assert " 1 1 " in log and " 2 1 " in log and " 3 1 " in log, log
    s.screenshot("touchpad", [(.1, .5, .9, .8, "40a060")])
    s.close()
